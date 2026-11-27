-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with Ada.Strings.Fixed;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr; use Cg_Expr;
with Lib_Binder;

package body Cg_Stmt.Generics is

   -- P3-5.3: True when Item is a DN_STUB whose body is inlined from Ctx.Gen_Subunits
   function Find_Gen_Subunit_Body (Ctx : Context; Item : TREE) return TREE is
   begin
      if Is_Void_Tree (Item) or else Is_Void_Tree (D (AS_BODY, Item)) or else KIND (D (AS_BODY, Item)) /= DN_STUB then
         return CONST_VOID;
      end if;
      if KIND (Item) = DN_TASK_BODY then
         declare
            TB_Id   : constant TREE := D (AS_SOURCE_NAME, Item);
            Orig_TS : constant TREE := (if not Is_Void_Tree (TB_Id) then Resolve_Type_Spec (D (SM_TYPE_SPEC, TB_Id)) else CONST_VOID);
            Sub_TS  : constant TREE := (if not Is_Void_Tree (TB_Id) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TB_Id))) else CONST_VOID);
         begin
            if not Is_Void_Tree (Orig_TS) and then Ctx.Gen_Subunits.Contains (Tree_Key (Orig_TS)) then
               return Ctx.Gen_Subunits.Element (Tree_Key (Orig_TS));
            elsif not Is_Void_Tree (Sub_TS) and then Ctx.Gen_Subunits.Contains (Tree_Key (Sub_TS)) then
               return Ctx.Gen_Subunits.Element (Tree_Key (Sub_TS));
            end if;
         end;
      elsif KIND (Item) in DN_SUBPROGRAM_BODY | DN_PACKAGE_BODY then
         declare
            Orig_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
            Sub_Id  : constant TREE := Canonical_Def (Subst_Tree (Ctx, Orig_Id));
         begin
            if not Is_Void_Tree (Orig_Id) and then Ctx.Gen_Subunits.Contains (Tree_Key (Orig_Id)) then
               return Ctx.Gen_Subunits.Element (Tree_Key (Orig_Id));
            elsif not Is_Void_Tree (Sub_Id) and then Ctx.Gen_Subunits.Contains (Tree_Key (Sub_Id)) then
               return Ctx.Gen_Subunits.Element (Tree_Key (Sub_Id));
            end if;
         end;
      end if;
      return CONST_VOID;
   end Find_Gen_Subunit_Body;

   function Is_Gen_Subunit_Stub (Ctx : Context; Item : TREE) return Boolean is
   begin
      return not Is_Void_Tree (Find_Gen_Subunit_Body (Ctx, Item));
   end Is_Gen_Subunit_Stub;

   -- P3-5.7: Verify a generic is truly a predefined RTL generic (not a user generic shadowing the same name!)
   function Is_RTL_Generic (Gen_Id : TREE) return Boolean is
      GN  : constant String := (if not Is_Void_Tree (Gen_Id) then Sym_Name (Gen_Id) else "");
      Pkg : constant String := (if not Is_Void_Tree (Gen_Id) then Enclosing_Package_Name (Gen_Id) else "");
   begin
      if Is_Void_Tree (Gen_Id) or else not Is_Library_Level (Gen_Id) then
         return False;
      end if;
      if GN = "INTEGER_IO" or else GN = "FLOAT_IO" or else GN = "FIXED_IO" or else GN = "ENUMERATION_IO" then
         return Pkg = "TEXT_IO";
      elsif GN = "SEQUENTIAL_IO" or else GN = "DIRECT_IO"
        or else GN = "UNCHECKED_DEALLOCATION" or else GN = "UNCHECKED_CONVERSION"
      then
         return Pkg = "" or else Pkg = "STANDARD";
      end if;
      return False;
   end Is_RTL_Generic;

   -- =========================================================================
   -- Static-Link Capture Pre-Scan
   -- =========================================================================
   -- Walks nested subprograms inside Owner_Key to find any references to
   -- variables owned by Owner_Key (or ancestors), populating Captured_Vars.

   type Cap_Inst_Entry is record
      IK  : Unsigned_32 := 0;
      GId : TREE := CONST_VOID;
   end record;
   type Cap_Inst_Stack_Arr is array (1 .. 16) of Cap_Inst_Entry;
   Cap_Inst_Stack : Cap_Inst_Stack_Arr;
   Cap_Inst_Depth : Natural := 0;

   procedure Scan_Tree_For_Captures
     (Ctx           : in out Context;
      Node          : TREE;
      Current_Subp  : Unsigned_32;
      Owner_Map     : Var_Maps.Map;
      Parent_Map    : in out Subp_Maps.Map;
      Captured_Vars : in out Cap_Vecs.Vector;
      Nested_Subps  : in out U32_Vecs.Vector;
      Target_Owner  : Unsigned_32;
      Type_Depth    : Natural := 0;
      Inst_Key      : Unsigned_32 := 0;
      Inst_Gen_Id   : TREE := CONST_VOID)
   is
      K : NODE_NAME;

      function Resolve_Cap_Inst_Key (Var_Def : TREE; Cur_Inst : Unsigned_32; Cur_Gen_Id : TREE) return Unsigned_32 is
         Var_Gen : TREE := CONST_VOID;
         Cur     : TREE := Var_Def;
         Walk_G  : TREE := Cur_Gen_Id;
         Walk_I  : Unsigned_32 := Cur_Inst;
      begin
         if Cur_Inst = 0 or else Is_Void_Tree (Var_Def) or else Is_Void_Tree (Cur_Gen_Id) then
            return Cur_Inst;
         end if;
         for Hop in 1 .. 16 loop
            exit when Is_Void_Tree (Cur) or else KIND (Cur) not in CLASS_SOURCE_NAME;
            declare
               Reg : constant TREE := D (XD_REGION, Cur);
            begin
               exit when Is_Void_Tree (Reg) or else KIND (Reg) not in CLASS_SOURCE_NAME;
               if KIND (Reg) = DN_GENERIC_ID then
                  Var_Gen := Canonical_Def (Reg);
                  exit;
               end if;
               Cur := Reg;
            end;
         end loop;
         if Is_Void_Tree (Var_Gen) then
            return Cur_Inst;
         end if;
         -- P3-5.1: Check active enclosing generic instantiation stack first (handles body-nested generics)
         for Idx in reverse 1 .. Cap_Inst_Depth loop
            declare
               SG : constant TREE := Cap_Inst_Stack (Idx).GId;
               Can_S : constant TREE := (if not Is_Void_Tree (SG) then Canonical_Def (SG) else CONST_VOID);
               Orig_S : TREE := Can_S;
            begin
               if not Is_Void_Tree (Can_S) and then Ctx.Generic_Decls.Contains (Tree_Key (Can_S)) then
                  declare
                     GD : constant TREE := Ctx.Generic_Decls.Element (Tree_Key (Can_S));
                  begin
                     if not Is_Void_Tree (GD) and then KIND (GD) = DN_GENERIC_DECL then
                        Orig_S := Canonical_Def (D (AS_SOURCE_NAME, GD));
                     end if;
                  end;
               end if;
               if Can_S = Var_Gen or else Orig_S = Var_Gen then
                  return Cap_Inst_Stack (Idx).IK;
               end if;
            end;
         end loop;
         for Hop in 1 .. 16 loop
            exit when Is_Void_Tree (Walk_G) or else KIND (Walk_G) not in CLASS_SOURCE_NAME;
            declare
               Can_G : constant TREE := Canonical_Def (Walk_G);
               Orig_G : TREE := Can_G;
            begin
               if Ctx.Generic_Decls.Contains (Tree_Key (Can_G)) then
                  declare
                     GD : constant TREE := Ctx.Generic_Decls.Element (Tree_Key (Can_G));
                  begin
                     if not Is_Void_Tree (GD) and then KIND (GD) = DN_GENERIC_DECL then
                        Orig_G := Canonical_Def (D (AS_SOURCE_NAME, GD));
                     end if;
                  end;
               end if;
               if Can_G = Var_Gen or else Orig_G = Var_Gen then
                  return Walk_I;
               end if;
               declare
                  Reg : constant TREE := D (XD_REGION, Can_G);
               begin
                  if not Is_Void_Tree (Reg) and then KIND (Reg) = DN_PACKAGE_ID then
                     declare
                        UD : constant TREE := D (SM_UNIT_DESC, Reg);
                     begin
                        if not Is_Void_Tree (UD) and then KIND (UD) = DN_INSTANTIATION then
                           Walk_I := Tree_Key (Canonical_Def (Reg));
                           Walk_G := Canonical_Def (Get_Name_Defn (D (AS_NAME, UD)));
                        else
                           exit;
                        end if;
                     end;
                  else
                     exit;
                  end if;
               end;
            end;
         end loop;
         return Cur_Inst;
      end Resolve_Cap_Inst_Key;

      procedure Scan_Type_Bounds (Ty : TREE) is
         RT : TREE;
      begin
         if Type_Depth >= 3 or else Is_Void_Tree (Ty) then
            return;
         end if;
         RT := Resolve_Type_Spec (Subst_Tree (Ctx, Ty));
         if Is_Void_Tree (RT) then
            return;
         end if;
         case KIND (RT) is
            when DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED =>
               declare
                  Rng : constant TREE := D (SM_RANGE, RT);
               begin
                  if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                     Scan_Tree_For_Captures (Ctx, D (AS_EXP1, Rng), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth + 1, Inst_Key, Inst_Gen_Id);
                     Scan_Tree_For_Captures (Ctx, D (AS_EXP2, Rng), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth + 1, Inst_Key, Inst_Gen_Id);
                  end if;
               end;
            when DN_ARRAY =>
               Scan_Type_Bounds (D (SM_COMP_TYPE, RT));
               declare
                  IS_N : constant TREE := D (SM_INDEX_S, RT);
               begin
                  if not Is_Void_Tree (IS_N) then
                     declare
                        IL : SEQ_TYPE := LIST (IS_N);
                        IT : TREE;
                     begin
                        while not IS_EMPTY (IL) loop
                           POP (IL, IT);
                           if not Is_Void_Tree (IT) and then KIND (IT) = DN_INDEX then
                              Scan_Type_Bounds (D (SM_TYPE_SPEC, IT));
                           else
                              Scan_Type_Bounds (IT);
                           end if;
                        end loop;
                     end;
                  end if;
               end;
            when DN_CONSTRAINED_ARRAY =>
               Scan_Type_Bounds (Base_Type_Spec (RT));
               declare
                  IS_N : constant TREE := D (SM_INDEX_SUBTYPE_S, RT);
               begin
                  if not Is_Void_Tree (IS_N) then
                     declare
                        IL : SEQ_TYPE := LIST (IS_N);
                        IT : TREE;
                     begin
                        while not IS_EMPTY (IL) loop
                           POP (IL, IT);
                           Scan_Type_Bounds (IT);
                        end loop;
                     end;
                  end if;
               end;
            when DN_ACCESS | DN_CONSTRAINED_ACCESS =>
               Scan_Type_Bounds (D (SM_DESIG_TYPE, RT));
            when DN_RECORD =>
               declare
                  DS_N : constant TREE := D (SM_DISCRIMINANT_S, RT);
                  CL_N : constant TREE := D (SM_COMP_LIST, RT);
               begin
                  if not Is_Void_Tree (DS_N) then
                     Scan_Tree_For_Captures (Ctx, DS_N, Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth + 1, Inst_Key, Inst_Gen_Id);
                  end if;
                  if not Is_Void_Tree (CL_N) then
                     Scan_Tree_For_Captures (Ctx, CL_N, Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth + 1, Inst_Key, Inst_Gen_Id);
                  end if;
               end;
            when DN_CONSTRAINED_RECORD =>
               Scan_Tree_For_Captures (Ctx, D (SM_NORMALIZED_DSCRMT_S, RT), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth + 1, Inst_Key, Inst_Gen_Id);
               Scan_Type_Bounds (Base_Type_Spec (RT));
            when others =>
               null;
         end case;
      end Scan_Type_Bounds;

      procedure Bind_Gen_Formals_For_Scan (Gen_Id : TREE; UK : TREE) is
         GDecl : TREE := CONST_VOID;
         GK    : constant Unsigned_32 := Tree_Key (Gen_Id);
      begin
         if Ctx.Generic_Decls.Contains (GK) then
            GDecl := Ctx.Generic_Decls.Element (GK);
         end if;
         declare
            Param_S  : constant TREE :=
              (if not Is_Void_Tree (GDecl) and then KIND (GDecl) = DN_GENERIC_DECL then D (AS_ITEM_S, GDecl)
               elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_GENERIC_PARAM_S, Gen_Id)
               else CONST_VOID);
            ADecls_N : constant TREE := D (SM_DECL_S, UK);
         begin
            if not Is_Void_Tree (Param_S) and then not Is_Void_Tree (ADecls_N) then
               declare
                  FL : SEQ_TYPE := LIST (Param_S);
                  AL : SEQ_TYPE := LIST (ADecls_N);
                  FI, AD : TREE;
               begin
                  while not IS_EMPTY (FL) and then not IS_EMPTY (AL) loop
                     POP (FL, FI);
                     if KIND (FI) = DN_IN then
                        declare
                           FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FI));
                           F_Id : TREE;
                        begin
                           while not IS_EMPTY (FNS) and then not IS_EMPTY (AL) loop
                              POP (FNS, F_Id);
                              POP (AL, AD);
                              if KIND (AD) = DN_CONSTANT_DECL then
                                 Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (F_Id)),
                                   Canonical_Def (HEAD (LIST (D (AS_SOURCE_NAME_S, AD)))));
                              end if;
                           end loop;
                        end;
                     elsif KIND (FI) = DN_IN_OUT then
                        declare
                           FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FI));
                           F_Id : TREE;
                        begin
                           while not IS_EMPTY (FNS) and then not IS_EMPTY (AL) loop
                              POP (FNS, F_Id);
                              POP (AL, AD);
                              if KIND (AD) = DN_RENAMES_OBJ_DECL then
                                 Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (F_Id)),
                                   Canonical_Def (D (AS_SOURCE_NAME, AD)));
                              end if;
                           end loop;
                        end;
                     else
                        POP (AL, AD);
                        if KIND (FI) = DN_TYPE_DECL and then KIND (AD) = DN_SUBTYPE_DECL then
                           declare
                              F_Tid : constant TREE := D (AS_SOURCE_NAME, FI);
                              F_Ts  : constant TREE := D (SM_TYPE_SPEC, F_Tid);
                              A_Sid : constant TREE := D (AS_SOURCE_NAME, AD);
                              A_Ts  : constant TREE := D (SM_TYPE_SPEC, A_Sid);
                           begin
                              Ctx.Generic_Subst.Include (Tree_Key (F_Tid), A_Sid);
                              if not Is_Void_Tree (F_Ts) and then not Is_Void_Tree (A_Ts) then
                                 Ctx.Generic_Subst.Include (Tree_Key (F_Ts), A_Ts);
                              end if;
                           end;
                        elsif KIND (FI) = DN_SUBPROG_ENTRY_DECL and then KIND (AD) = DN_SUBPROG_ENTRY_DECL then
                           declare
                              AUK : constant TREE := D (AS_UNIT_KIND, AD);
                           begin
                              if not Is_Void_Tree (AUK) and then KIND (AUK) = DN_RENAMES_UNIT then
                                 Ctx.Generic_Subst.Include
                                   (Tree_Key (Canonical_Def (D (AS_SOURCE_NAME, FI))),
                                    Canonical_Def (D (AS_SOURCE_NAME, AD)));
                              end if;
                           end;
                        end if;
                     end if;
                  end loop;
               end;
            end if;
         end;
      end Bind_Gen_Formals_For_Scan;

      procedure Register_Nested_Child (CK : Unsigned_32) is
         Found_N : Boolean := False;
      begin
         for NK of Nested_Subps loop
            if NK = CK then Found_N := True; exit; end if;
         end loop;
         if not Found_N then
            Nested_Subps.Append (CK);
         end if;
         if not Parent_Map.Contains (CK) then
            declare
               SI : Subp_Info;
            begin
               SI.Parent_Subp := Current_Subp;
               Parent_Map.Insert (CK, SI);
            end;
         else
            declare
               SI : Subp_Info := Parent_Map.Element (CK);
            begin
               SI.Parent_Subp := Current_Subp;
               Parent_Map.Replace (CK, SI);
            end;
         end if;
      end Register_Nested_Child;
   begin
      if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then
         return;
      end if;
      K := KIND (Node);
      if K not in CLASS_ALL_SOURCE then
         return;
      end if;

      if K = DN_USED_OBJECT_ID or else K = DN_USED_NAME_ID then
         declare
            Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Node)));
            DK  : constant Unsigned_32 := Tree_Key (Def);
         begin
            if Current_Subp /= Target_Owner then
               if not Is_Void_Tree (Def) and then KIND (Def) in CLASS_TYPE_NAME then
                  Scan_Type_Bounds (D (SM_TYPE_SPEC, Def));
               elsif not Is_Void_Tree (Def) and then KIND (Def) in CLASS_OBJECT_NAME then
                  Scan_Type_Bounds (D (SM_OBJ_TYPE, Def));
               end if;
            end if;
            if Current_Subp /= Target_Owner and then Owner_Map.Contains (DK) then
               declare
                  V : constant Var_Info := Owner_Map.Element (DK);
                  Eff_Inst : constant Unsigned_32 :=
                    (if V.Captured_Slot = 1 then Resolve_Cap_Inst_Key (Def, Inst_Key, Inst_Gen_Id) else 0);
               begin
                  if V.Owner_Subp = Target_Owner then
                     -- Record captured variable if not already listed
                     declare
                        Found : Boolean := False;
                     begin
                        for E of Captured_Vars loop
                           if E.Var_Key = DK and then E.Inst_Key = Eff_Inst then Found := True; exit; end if;
                        end loop;
                        if not Found then
                           Captured_Vars.Append ((Var_Key => DK, Inst_Key => Eff_Inst));
                        end if;
                     end;
                     -- Mark Current_Subp and its ancestors up to Target_Owner as needing SL
                     declare
                        Walk_K : Unsigned_32 := Current_Subp;
                     begin
                        for Step in 1 .. 16 loop
                           exit when Walk_K = 0 or else Walk_K = Target_Owner;
                           exit when not Parent_Map.Contains (Walk_K);
                           declare
                              SI : Subp_Info := Parent_Map.Element (Walk_K);
                           begin
                              SI.Needs_SL := True;
                              Parent_Map.Replace (Walk_K, SI);
                              Walk_K := SI.Parent_Subp;
                           end;
                        end loop;
                     end;
                  end if;
               end;
            end if;
         end;
         return;
      end if;

      if K = DN_SUBPROG_ENTRY_DECL then
         declare
            UK : constant TREE := D (AS_UNIT_KIND, Node);
         begin
            if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
               declare
                  Inst_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                  IK      : constant Unsigned_32 := Tree_Key (Inst_Id);
                  Gen_Id  : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                  GK      : constant Unsigned_32 := Tree_Key (Gen_Id);
                  Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
               begin
                  if IK /= 0 and then IK /= Current_Subp then
                     Register_Nested_Child (IK);
                     Scan_Tree_For_Captures (Ctx, D (SM_DECL_S, UK), IK, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
                     Bind_Gen_Formals_For_Scan (Gen_Id, UK);
                     declare
                        GB : TREE := CONST_VOID;
                     begin
                        if Ctx.Generic_Body.Contains (GK) then
                           GB := Ctx.Generic_Body.Element (GK);
                        elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then
                           GB := D (SM_BODY, Gen_Id);
                           if Is_Void_Tree (GB) then
                              GB := D (XD_BODY, Gen_Id);
                           end if;
                        end if;
                        if not Is_Void_Tree (GB) and then KIND (GB) = DN_SUBPROGRAM_BODY then
                           declare
                              Saved_D : constant Natural := Cap_Inst_Depth;
                           begin
                              if Cap_Inst_Depth < Cap_Inst_Stack'Last then
                                 Cap_Inst_Depth := Cap_Inst_Depth + 1;
                                 Cap_Inst_Stack (Cap_Inst_Depth) := (IK => IK, GId => Gen_Id);
                              end if;
                              Scan_Tree_For_Captures (Ctx, D (AS_BODY, GB), IK, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, IK, Gen_Id);
                              Cap_Inst_Depth := Saved_D;
                           end;
                        end if;
                     end;
                     Ctx.Generic_Subst := Saved_Sub;
                     return;
                  end if;
               end;
            end if;
         end;
      end if;

      if K = DN_PACKAGE_DECL then
         declare
            UK : constant TREE := D (AS_UNIT_KIND, Node);
         begin
            if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
               declare
                  PID    : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                  PIK    : constant Unsigned_32 := Tree_Key (PID);
                  Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                  GK     : constant Unsigned_32 := Tree_Key (Gen_Id);
                  GB     : TREE := CONST_VOID;
                  Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
               begin
                  -- Packages do not have their own static-link frame; scan their decls & generic body under Current_Subp!
                  Scan_Tree_For_Captures (Ctx, D (SM_DECL_S, UK), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
                  declare
                     Saved_D : constant Natural := Cap_Inst_Depth;
                  begin
                     if Cap_Inst_Depth < Cap_Inst_Stack'Last then
                        Cap_Inst_Depth := Cap_Inst_Depth + 1;
                        Cap_Inst_Stack (Cap_Inst_Depth) := (IK => PIK, GId => Gen_Id);
                     end if;
                     Scan_Tree_For_Captures (Ctx, D (SM_SPEC, PID), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, PIK, Gen_Id);
                     Bind_Gen_Formals_For_Scan (Gen_Id, UK);
                     declare
                        GSp : constant TREE := (if not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_SPEC, Gen_Id) else CONST_VOID);
                        ISp : constant TREE := D (SM_SPEC, PID);
                     begin
                        if not Is_Void_Tree (GSp) and then KIND (GSp) = DN_PACKAGE_SPEC
                          and then not Is_Void_Tree (ISp) and then KIND (ISp) = DN_PACKAGE_SPEC
                        then
                           Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, GSp), D (AS_DECL_S1, ISp));
                           Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, GSp), D (AS_DECL_S2, ISp));
                        end if;
                     end;
                     if Ctx.Generic_Body.Contains (GK) then
                        GB := Ctx.Generic_Body.Element (GK);
                     elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then
                        GB := D (SM_BODY, Gen_Id);
                        if Is_Void_Tree (GB) then
                           GB := D (XD_BODY, Gen_Id);
                        end if;
                     end if;
                     if not Is_Void_Tree (GB) then
                        if KIND (GB) = DN_PACKAGE_BODY then
                           Scan_Tree_For_Captures (Ctx, D (AS_BODY, GB), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, PIK, Gen_Id);
                        else
                           Scan_Tree_For_Captures (Ctx, GB, Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, PIK, Gen_Id);
                        end if;
                     end if;
                     Cap_Inst_Depth := Saved_D;
                  end;
                  Ctx.Generic_Subst := Saved_Sub;
                  return;
               end;
            end if;
         end;
      end if;

      if K = DN_INSTANTIATION then
         Scan_Tree_For_Captures (Ctx, D (SM_DECL_S, Node), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
      end if;

      if K = DN_TASK_BODY then
         declare
            TB_Id : constant TREE := D (AS_SOURCE_NAME, Node);
            TS    : constant TREE := (if not Is_Void_Tree (TB_Id) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TB_Id))) else CONST_VOID);
            TK    : constant Unsigned_32 := (if not Is_Void_Tree (TS) then Tree_Key (TS) else 0);
            Sub_B : constant TREE := Find_Gen_Subunit_Body (Ctx, Node);
            Scan_B : constant TREE := (if not Is_Void_Tree (Sub_B) then D (AS_BODY, Sub_B) else D (AS_BODY, Node));
         begin
            if TK /= 0 and then TK /= Current_Subp then
               Register_Nested_Child (TK);
               Scan_Tree_For_Captures (Ctx, D (SM_DECL_S, TS), TK, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
               Scan_Tree_For_Captures (Ctx, Scan_B, TK, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
               return;
            end if;
         end;
      end if;

      if K = DN_PACKAGE_BODY then
         declare
            Orig_P : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
            Sub_B  : constant TREE := Find_Gen_Subunit_Body (Ctx, Node);
         begin
            -- P3-5.1: Skip uninstantiated nested generic package bodies during outer capture scan
            if not Is_Void_Tree (Orig_P) and then KIND (Orig_P) = DN_GENERIC_ID then
               return;
            end if;
            if not Is_Void_Tree (Sub_B) then
               Scan_Tree_For_Captures (Ctx, D (AS_BODY, Sub_B), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
               return;
            end if;
         end;
      end if;

      if K = DN_SUBPROGRAM_BODY then
         -- P3-5.1: Skip uninstantiated nested generic subprogram bodies during outer capture scan
         declare
            Orig_S : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
            Sub_B  : constant TREE := Find_Gen_Subunit_Body (Ctx, Node);
         begin
            if not Is_Void_Tree (Orig_S) and then KIND (Orig_S) = DN_GENERIC_ID then
               return;
            end if;
            -- LRM 10.2 (c83022g): external separate subunit stubs never receive a parent static link;
            -- P3-5.3: inlined generic subunit stubs DO participate in capture analysis!
            if not Is_Void_Tree (D (AS_BODY, Node)) and then KIND (D (AS_BODY, Node)) = DN_STUB and then Is_Void_Tree (Sub_B) then
               return;
            end if;
            declare
               Subp_Id : constant TREE := Canonical_Def (Subst_Tree (Ctx, Orig_S));
               SK      : constant Unsigned_32 := Tree_Key (Subp_Id);
               Scan_B  : constant TREE := (if not Is_Void_Tree (Sub_B) then D (AS_BODY, Sub_B) else D (AS_BODY, Node));
            begin
               if SK /= Current_Subp then
                  Register_Nested_Child (SK);
                  Scan_Tree_For_Captures (Ctx, D (AS_HEADER, Node), SK, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
                  Scan_Tree_For_Captures (Ctx, Scan_B, SK, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
                  return;
               end if;
            end;
         end;
      end if;

      if KIND (Node) in DN_PROCEDURE_CALL | DN_FUNCTION_CALL | DN_ENTRY_CALL then
         Scan_Tree_For_Captures (Ctx, D (AS_NAME, Node), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
         declare
            NPS : constant TREE := D (SM_NORMALIZED_PARAM_S, Node);
         begin
            if not Is_Void_Tree (NPS) then
               Scan_Tree_For_Captures (Ctx, NPS, Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
            else
               Scan_Tree_For_Captures (Ctx, D (AS_GENERAL_ASSOC_S, Node), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
            end if;
         end;
         return;
      end if;

      if ARITY (Node) = ARBITRARY then
         declare
            L : SEQ_TYPE := LIST (Node);
            Item : TREE;
         begin
            while not IS_EMPTY (L) loop
               POP (L, Item);
               Scan_Tree_For_Captures (Ctx, Item, Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
            end loop;
         end;
      else
         for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
            Scan_Tree_For_Captures (Ctx, DABS (I, Node), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
         end loop;
      end if;
   end Scan_Tree_For_Captures;

   -- =========================================================================
   -- Secondary Stack Pre-Scan & Lifetime Helpers
   -- =========================================================================

   function Tree_Uses_SS (Ctx : in out Context; Node : TREE) return Boolean is
      K : NODE_NAME;
   begin
      if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then
         return False;
      end if;
      K := KIND (Node);
      if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then
         return False;
      end if;

      if K = DN_FUNCTION_CALL or else K = DN_NULLARY_CALL then
         declare
            ET : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
         begin
            if not Is_Void_Tree (ET) then
               declare
                  TI : constant Type_Info := Get_Type_Info (Ctx, ET);
               begin
                  if TI.Kind = TK_Unconstrained_Array
                    or else (TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic)
                  then
                     return True;
                  end if;
               end;
            end if;
            declare
               NN : TREE := D (AS_NAME, Node);
            begin
               if not Is_Void_Tree (NN) then
                  if KIND (NN) = DN_SELECTED then
                     NN := D (AS_DESIGNATOR, NN);
                  end if;
                  if KIND (NN) = DN_USED_OP then
                     declare
                        Def : constant TREE := D (SM_DEFN, NN);
                     begin
                        if not Is_Void_Tree (Def) and then KIND (Def) = DN_BLTN_OPERATOR_ID then
                           declare
                              Op_C : constant Integer := DI (SM_OPERATOR, Def);
                           begin
                              if Op_C = 19
                                or else (Op_C in 0 | 1 | 2 | 3 and then not Is_Void_Tree (ET) and then Is_Array_Type (ET))
                              then
                                 return True;
                              end if;
                           end;
                        end if;
                     end;
                  elsif not Is_Void_Tree (ET) and then Is_Array_Type (ET)
                    and then KIND (NN) in DN_USED_OBJECT_ID | DN_USED_NAME_ID
                  then
                     declare
                        Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, NN)));
                     begin
                        if not Is_Void_Tree (Def) and then KIND (Def) in DN_FUNCTION_ID | DN_OPERATOR_ID
                          and then not Is_Void_Tree (D (SM_UNIT_DESC, Def))
                          and then KIND (D (SM_UNIT_DESC, Def)) = DN_INSTANTIATION
                        then
                           declare
                              GId : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, D (SM_UNIT_DESC, Def))));
                           begin
                              if Is_RTL_Generic (GId) and then Sym_Name (GId) = "UNCHECKED_CONVERSION" then
                                 return True;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end if;
            end;
         end;
         if K = DN_FUNCTION_CALL and then not Is_Void_Tree (D (SM_NORMALIZED_PARAM_S, Node)) then
            if Tree_Uses_SS (Ctx, D (SM_NORMALIZED_PARAM_S, Node)) then
               return True;
            end if;
         end if;
      elsif K in DN_PROCEDURE_CALL | DN_ENTRY_CALL then
         if not Is_Void_Tree (D (SM_NORMALIZED_PARAM_S, Node)) then
            if Tree_Uses_SS (Ctx, D (SM_NORMALIZED_PARAM_S, Node)) then
               return True;
            end if;
         end if;
      elsif K = DN_AGGREGATE or else K = DN_QUALIFIED_ALLOCATOR then
         return True;
      elsif K = DN_ATTRIBUTE then
         declare
            AD : constant TREE := D (AS_USED_NAME_ID, Node);
         begin
            if not Is_Void_Tree (AD) and then Sym_Name (AD) = "IMAGE" then
               return True;
            end if;
         end;
      elsif K = DN_VARIABLE_DECL or else K = DN_CONSTANT_DECL then
         declare
            NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Node));
            ID : TREE;
         begin
            if not IS_EMPTY (NS) then
               POP (NS, ID);
               declare
                  TI : constant Type_Info := Get_Type_Info (Ctx, D (SM_OBJ_TYPE, ID));
               begin
                  if TI.Kind = TK_Unconstrained_Array
                    or else (TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic)
                  then
                     return True;
                  end if;
               end;
            end if;
         end;
      end if;

      if ARITY (Node) = ARBITRARY then
         declare
            L    : SEQ_TYPE := LIST (Node);
            Item : TREE;
         begin
            while not IS_EMPTY (L) loop
               POP (L, Item);
               if Tree_Uses_SS (Ctx, Item) then
                  return True;
               end if;
            end loop;
         end;
      else
         for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
            if Tree_Uses_SS (Ctx, DABS (I, Node)) then
               return True;
            end if;
         end loop;
      end if;
      return False;
   end Tree_Uses_SS;

   function Tree_Has_Return (Node : TREE) return Boolean is
      K : NODE_NAME;
   begin
      if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then
         return False;
      end if;
      K := KIND (Node);
      if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then
         return False;
      end if;
      if K = DN_RETURN then
         return True;
      end if;
      if ARITY (Node) = ARBITRARY then
         declare
            L    : SEQ_TYPE := LIST (Node);
            Item : TREE;
         begin
            while not IS_EMPTY (L) loop
               POP (L, Item);
               if Tree_Has_Return (Item) then
                  return True;
               end if;
            end loop;
         end;
      else
         for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
            if Tree_Has_Return (DABS (I, Node)) then
               return True;
            end if;
         end loop;
      end if;
      return False;
   end Tree_Has_Return;

   -- =========================================================================
   -- Generic Instantiation Emission
   -- =========================================================================

   procedure Map_Decl_List_Ids
     (Ctx      : in out Context;
      Gen_DS   : TREE;
      Inst_DS  : TREE)
   is
      GL, IL : SEQ_TYPE;
      GD, ID : TREE;
   begin
      if Is_Void_Tree (Gen_DS) or else Is_Void_Tree (Inst_DS) then
         return;
      end if;
      GL := LIST (Gen_DS);
      IL := LIST (Inst_DS);
      while not IS_EMPTY (GL) and then not IS_EMPTY (IL) loop
         POP (GL, GD);
         POP (IL, ID);
         case KIND (GD) is
            when DN_RENAMES_EXC_DECL | DN_RENAMES_OBJ_DECL =>
               if KIND (ID) = KIND (GD) then
                  declare
                     GN   : constant TREE := D (AS_SOURCE_NAME, GD);
                     IN_N : constant TREE := D (AS_SOURCE_NAME, ID);
                  begin
                     if not Is_Void_Tree (GN) and then not Is_Void_Tree (IN_N) then
                        Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (GN)), Canonical_Def (IN_N));
                        Ctx.Generic_Subst.Include (Tree_Key (GN), Canonical_Def (IN_N));
                     end if;
                  end;
               end if;
            when DN_VARIABLE_DECL | DN_CONSTANT_DECL | DN_EXCEPTION_DECL =>
               if KIND (ID) = KIND (GD) then
                  declare
                     GNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, GD));
                     INS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, ID));
                     GN, IN_N : TREE;
                  begin
                     while not IS_EMPTY (GNS) and then not IS_EMPTY (INS) loop
                        POP (GNS, GN); POP (INS, IN_N);
                        Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (GN)), Canonical_Def (IN_N));
                        Ctx.Generic_Subst.Include (Tree_Key (GN), Canonical_Def (IN_N));
                     end loop;
                  end;
               end if;
            when DN_TYPE_DECL | DN_SUBTYPE_DECL =>
               if KIND (ID) = KIND (GD) then
                  declare
                     GN : constant TREE := D (AS_SOURCE_NAME, GD);
                     IN_N : constant TREE := D (AS_SOURCE_NAME, ID);
                     GT : constant TREE := D (SM_TYPE_SPEC, GN);
                     IT : constant TREE := D (SM_TYPE_SPEC, IN_N);
                  begin
                     Ctx.Generic_Subst.Include (Tree_Key (GN), IN_N);
                     if not Is_Void_Tree (GT) and then not Is_Void_Tree (IT) then
                        Ctx.Generic_Subst.Include (Tree_Key (GT), IT);
                        declare
                           RGT : constant TREE := Resolve_Type_Spec (GT);
                           RIT : constant TREE := Resolve_Type_Spec (IT);
                           procedure Map_Disc_Pair (Src_T : TREE; Dst_T : TREE) is
                           begin
                              if not Is_Void_Tree (Src_T) and then not Is_Void_Tree (Dst_T)
                                and then KIND (Src_T) in DN_RECORD | DN_PRIVATE | DN_L_PRIVATE
                                and then KIND (Dst_T) in DN_RECORD | DN_PRIVATE | DN_L_PRIVATE
                                and then not Is_Void_Tree (D (SM_DISCRIMINANT_S, Src_T))
                                and then not Is_Void_Tree (D (SM_DISCRIMINANT_S, Dst_T))
                              then
                                 declare
                                    GL : SEQ_TYPE := LIST (D (SM_DISCRIMINANT_S, Src_T));
                                    IL : SEQ_TYPE := LIST (D (SM_DISCRIMINANT_S, Dst_T));
                                    GD_D, ID_D : TREE;
                                 begin
                                    while not IS_EMPTY (GL) and then not IS_EMPTY (IL) loop
                                       POP (GL, GD_D); POP (IL, ID_D);
                                       if KIND (GD_D) = DN_DSCRMT_DECL and then KIND (ID_D) = DN_DSCRMT_DECL then
                                          declare
                                             GNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, GD_D));
                                             INS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, ID_D));
                                             GDI, IDI : TREE;
                                          begin
                                             while not IS_EMPTY (GNS) and then not IS_EMPTY (INS) loop
                                                POP (GNS, GDI); POP (INS, IDI);
                                                Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (GDI)), Canonical_Def (IDI));
                                                Ctx.Generic_Subst.Include (Tree_Key (GDI), Canonical_Def (IDI));
                                             end loop;
                                          end;
                                       end if;
                                    end loop;
                                 end;
                              end if;
                           end Map_Disc_Pair;
                           procedure Map_Comp_List_Pair (Src_CL, Dst_CL : TREE) is
                           begin
                              if Is_Void_Tree (Src_CL) or else Is_Void_Tree (Dst_CL) then return; end if;
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S, Src_CL), D (AS_DECL_S, Dst_CL));
                              declare
                                 SVP : constant TREE := D (AS_VARIANT_PART, Src_CL);
                                 DVP : constant TREE := D (AS_VARIANT_PART, Dst_CL);
                              begin
                                 if not Is_Void_Tree (SVP) and then KIND (SVP) = DN_VARIANT_PART
                                   and then not Is_Void_Tree (DVP) and then KIND (DVP) = DN_VARIANT_PART
                                   and then not Is_Void_Tree (D (AS_VARIANT_S, SVP))
                                   and then not Is_Void_Tree (D (AS_VARIANT_S, DVP))
                                 then
                                    declare
                                       SVL : SEQ_TYPE := LIST (D (AS_VARIANT_S, SVP));
                                       DVL : SEQ_TYPE := LIST (D (AS_VARIANT_S, DVP));
                                       SV, DV : TREE;
                                    begin
                                       while not IS_EMPTY (SVL) and then not IS_EMPTY (DVL) loop
                                          POP (SVL, SV); POP (DVL, DV);
                                          if KIND (SV) = DN_VARIANT and then KIND (DV) = DN_VARIANT then
                                             Map_Comp_List_Pair (D (AS_COMP_LIST, SV), D (AS_COMP_LIST, DV));
                                          end if;
                                       end loop;
                                    end;
                                 end if;
                              end;
                           end Map_Comp_List_Pair;
                        begin
                           Map_Disc_Pair (GT, IT);
                           Map_Disc_Pair (GT, RIT);
                           Map_Disc_Pair (RGT, RIT);
                           if not Is_Void_Tree (RGT) and then KIND (RGT) = DN_RECORD
                             and then not Is_Void_Tree (RIT) and then KIND (RIT) = DN_RECORD
                           then
                              Map_Comp_List_Pair (D (SM_COMP_LIST, RGT), D (SM_COMP_LIST, RIT));
                           end if;
                        end;
                     end if;
                  end;
               end if;
            when DN_SUBPROG_ENTRY_DECL =>
               if KIND (ID) = DN_SUBPROG_ENTRY_DECL then
                  declare
                     GN : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, GD));
                     IN_N : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, ID));
                  begin
                     Ctx.Generic_Subst.Include (Tree_Key (GN), IN_N);
                  end;
               end if;
            when DN_PACKAGE_DECL =>
               if KIND (ID) = DN_PACKAGE_DECL then
                  declare
                     GN : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, GD));
                     IN_N : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, ID));
                     GSpec : constant TREE := D (SM_SPEC, GN);
                     ISpec : constant TREE := D (SM_SPEC, IN_N);
                  begin
                     Ctx.Generic_Subst.Include (Tree_Key (GN), IN_N);
                     if not Is_Void_Tree (GSpec) and then KIND (GSpec) = DN_PACKAGE_SPEC
                       and then not Is_Void_Tree (ISpec) and then KIND (ISpec) = DN_PACKAGE_SPEC
                     then
                        Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, GSpec), D (AS_DECL_S1, ISpec));
                        Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, GSpec), D (AS_DECL_S2, ISpec));
                     end if;
                  end;
               end if;
            when DN_TASK_DECL =>
               if KIND (ID) = DN_TASK_DECL then
                  declare
                     GSrc : constant TREE := D (AS_SOURCE_NAME, GD);
                     ISrc : constant TREE := D (AS_SOURCE_NAME, ID);
                     GTS  : TREE := CONST_VOID;
                     ITS  : TREE := CONST_VOID;
                  begin
                     if not Is_Void_Tree (GSrc) and then not Is_Void_Tree (ISrc) then
                        Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (GSrc)), Canonical_Def (ISrc));
                        Ctx.Generic_Subst.Include (Tree_Key (GSrc), Canonical_Def (ISrc));
                        if KIND (GSrc) = DN_TYPE_ID then GTS := D (SM_TYPE_SPEC, GSrc);
                        elsif KIND (GSrc) = DN_VARIABLE_ID then GTS := D (SM_OBJ_TYPE, GSrc);
                        end if;
                        if KIND (ISrc) = DN_TYPE_ID then ITS := D (SM_TYPE_SPEC, ISrc);
                        elsif KIND (ISrc) = DN_VARIABLE_ID then ITS := D (SM_OBJ_TYPE, ISrc);
                        end if;
                        if not Is_Void_Tree (GTS) and then not Is_Void_Tree (ITS) then
                           Ctx.Generic_Subst.Include (Tree_Key (GTS), ITS);
                           if KIND (GTS) = DN_TASK_SPEC and then KIND (ITS) = DN_TASK_SPEC then
                              Map_Decl_List_Ids (Ctx, D (SM_DECL_S, GTS), D (SM_DECL_S, ITS));
                           end if;
                        end if;
                     end if;
                  end;
               end if;
            when DN_GENERIC_DECL =>
               if KIND (ID) = DN_GENERIC_DECL then
                  declare
                     GN   : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, GD));
                     IN_N : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, ID));
                     GHdr : constant TREE := D (AS_HEADER, GD);
                     IHdr : constant TREE := D (AS_HEADER, ID);
                  begin
                     Ctx.Generic_Subst.Include (Tree_Key (GN), IN_N);
                     Ctx.Generic_Decls.Include (Tree_Key (IN_N), GD);
                     -- P3-5.1: Propagate nested generic body to instantiated nested generic ID
                     if Ctx.Generic_Body.Contains (Tree_Key (GN)) then
                        Ctx.Generic_Body.Include (Tree_Key (IN_N), Ctx.Generic_Body.Element (Tree_Key (GN)));
                     elsif KIND (GN) = DN_GENERIC_ID then
                        if not Is_Void_Tree (D (SM_BODY, GN)) then
                           Ctx.Generic_Body.Include (Tree_Key (IN_N), D (SM_BODY, GN));
                        elsif not Is_Void_Tree (D (XD_BODY, GN)) then
                           Ctx.Generic_Body.Include (Tree_Key (IN_N), D (XD_BODY, GN));
                        end if;
                     end if;
                     if not Is_Void_Tree (GHdr) and then KIND (GHdr) = DN_PACKAGE_SPEC
                       and then not Is_Void_Tree (IHdr) and then KIND (IHdr) = DN_PACKAGE_SPEC
                     then
                        Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, GHdr), D (AS_DECL_S1, IHdr));
                        Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, GHdr), D (AS_DECL_S2, IHdr));
                     end if;
                  end;
               end if;
            when others =>
               null;
         end case;
      end loop;
   end Map_Decl_List_Ids;

   procedure Emit_Generic_Instantiation
     (Ctx        : in out Context;
      Inst_Decl  : TREE;
      Export_All : Boolean)
   is
      Inst_Id  : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Inst_Decl));
      Unit_K   : constant TREE := D (AS_UNIT_KIND, Inst_Decl);
      Gen_Id   : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, Unit_K)));
      Gen_Key  : constant Unsigned_32 := Tree_Key (Gen_Id);
      Gen_Name : constant String := Sym_Name (Gen_Id);
      Param_S  : TREE := CONST_VOID;
      GBody    : TREE := CONST_VOID;
   begin
      -- Skip predefined RTL generic instantiations handled directly by runtime (P3-5.7: verify enclosing pkg!)
      if Is_RTL_Generic (Gen_Id) then
         -- Still emit instantiated RTL package spec vars (e.g. DEFAULT_WIDTH, DEFAULT_BASE, DEFAULT_FORE, DEFAULT_AFT)
         -- using local vs global storage matching the enclosing scope!
         if KIND (Inst_Decl) = DN_PACKAGE_DECL then
            declare
               ISpec     : constant TREE := D (SM_SPEC, Inst_Id);
               RTL_Glob  : constant Boolean := (Ctx.Cur_Subp_Key = 0) or else Ctx.Has_Stubs;
               Dummy_Dyn : Boolean;
            begin
               if not Is_Void_Tree (ISpec) and then KIND (ISpec) = DN_PACKAGE_SPEC then
                  Emit_Decl_Items (Ctx, D (AS_DECL_S1, ISpec), Is_Global => RTL_Glob, Export_All => (Export_All and then RTL_Glob), Has_Dyn => Dummy_Dyn);
                  Emit_Decl_Items (Ctx, D (AS_DECL_S2, ISpec), Is_Global => RTL_Glob, Export_All => False, Has_Dyn => Dummy_Dyn);
               end if;
            end;
         end if;
         return;
      end if;

      if Ctx.Generic_Decls.Contains (Gen_Key) then
         Param_S := D (AS_ITEM_S, Ctx.Generic_Decls.Element (Gen_Key));
      elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then
         Param_S := D (SM_GENERIC_PARAM_S, Gen_Id);
      end if;

      if Ctx.Generic_Body.Contains (Gen_Key) then
         GBody := Ctx.Generic_Body.Element (Gen_Key);
      elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then
         declare
            XB : constant TREE := D (XD_BODY, Gen_Id);
            SB : constant TREE := D (SM_BODY, Gen_Id);
         begin
            if not Is_Void_Tree (XB) then
               GBody := XB;
            elsif not Is_Void_Tree (SB) then
               GBody := SB;
            end if;
         end;
      end if;

      -- Built-in support for ACVC Chapter 13 library generics LENGTH_CHECK & ENUM_CHECK
      -- (whose bodies live in separate .BDY files not loaded by libphase)
      if Is_Void_Tree (GBody) and then (Gen_Name = "LENGTH_CHECK" or else Gen_Name = "ENUM_CHECK") then
         declare
            Inst_Spec : constant TREE := D (SM_SPEC, Inst_Id);
            SI        : Subp_Info;
            Saved_Buf : Pir_Emit.Func_Buffer;
         begin
            Pre_Register_Subp (Ctx, Inst_Id, Inst_Spec, Ctx.Cur_Subp_Key);
            SI := Ctx.Subps.Element (Tree_Key (Canonical_Def (Inst_Id)));
            declare
               Link   : constant String := (if Export_All then "export " else "");
               Params : constant String :=
                 Build_Param_Sig (Ctx, Inst_Spec, SI.Needs_SL, False, CONST_VOID, True);
            begin
               Begin_Func (Ctx.E, Saved_Buf,
                 Link & "func " & To_String (SI.Pir_Name) & "(" & Params & ") -> void cc ""internal""");
               Emit_Term (Ctx.E, "ret");
               End_Func (Ctx.E, Saved_Buf);
            end;
         end;
         return;
      end if;

      -- LRM 3.9: check generic body has been elaborated before instantiation (c39007a, c39007b; P3-5.16)
      if Ctx.Enable_Checks
        and then (KIND (Inst_Decl) /= DN_PACKAGE_DECL or else not Is_Void_Tree (GBody)
                  or else (not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID
                           and then not Is_Void_Tree (D (SM_SPEC, Gen_Id))
                           and then KIND (D (SM_SPEC, Gen_Id)) = DN_PACKAGE_SPEC
                           and then (Lib_Binder.Decl_List_Requires_Body (D (AS_DECL_S1, D (SM_SPEC, Gen_Id)))
                                     or else Lib_Binder.Decl_List_Requires_Body (D (AS_DECL_S2, D (SM_SPEC, Gen_Id)), Is_Private_Part => True))))
      then
         declare
            GSym_U : Unbounded_String;
         begin
            if Ctx.Gen_Elab_Flags.Contains (Gen_Key) then
               GSym_U := Ctx.Gen_Elab_Flags.Element (Gen_Key);
            elsif not Is_Void_Tree (Gen_Id) and then Is_Library_Level (Gen_Id)
              and then Gen_Name /= "LENGTH_CHECK" and then Gen_Name /= "ENUM_CHECK"
              and then Enclosing_Package_Name (Gen_Id) /= "TEXT_IO"
            then
               declare
                  S : constant String := "@ada_gelab__" & Qualified_Ada_Name (Gen_Id);
               begin
                  Add_Global_Decl (Ctx.E, S, "extern " & S & " = global i8, align 1");
                  GSym_U := To_Unbounded_String (S);
               end;
            end if;
            if Length (GSym_U) > 0 then
               declare
                  GSym  : constant String := To_String (GSym_U);
                  FA    : constant String := Next_Reg (Ctx.E, "gfa");
                  FV    : constant String := Next_Reg (Ctx.E, "gfv");
                  NE    : constant String := Next_Reg (Ctx.E, "gne");
                  L_Bad : constant String := Next_Label (Ctx.E, "gpe_b");
                  L_Ok  : constant String := Next_Label (Ctx.E, "gpe_o");
               begin
                  Emit_Inst (Ctx.E, FA & " = addr " & GSym);
                  Emit_Inst (Ctx.E, FV & " = load.i8 " & FA & ", align 1");
                  Emit_Inst (Ctx.E, NE & " = cmp_eq.i8 " & FV & ", 0");
                  Emit_Term (Ctx.E, "br " & NE & ", " & L_Bad & ", " & L_Ok);
                  Emit_Label (Ctx.E, L_Bad);
                  Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
                  Emit_Label (Ctx.E, L_Ok);
               end;
            end if;
         end;
      end if;

      if Is_Void_Tree (GBody) and then KIND (Inst_Decl) /= DN_PACKAGE_DECL then
         -- Emit a stub function raising PROGRAM_ERROR so dead-code calls verify in pirc (c39007b; P3-5.11 clear Handlers!)
         declare
            Inst_Spec : constant TREE := D (SM_SPEC, Inst_Id);
            SI        : Subp_Info;
            Saved_Buf : Pir_Emit.Func_Buffer;
            Saved_Hdl : constant Handler_Vecs.Vector := Ctx.Handlers;
         begin
            Pre_Register_Subp (Ctx, Inst_Id, Inst_Spec, Ctx.Cur_Subp_Key);
            SI := Ctx.Subps.Element (Tree_Key (Canonical_Def (Inst_Id)));
            declare
               Link   : constant String := (if Export_All then "export " else "");
               Params : constant String :=
                 Build_Param_Sig (Ctx, Inst_Spec, SI.Needs_SL, SI.Ret_Composite, SI.Ret_Type, True);
               Ret_S  : constant String :=
                 (if not SI.Is_Function or else SI.Ret_Composite then "void"
                  else To_String (Get_Type_Info (Ctx, SI.Ret_Type).Pir_Type));
            begin
               Ctx.Handlers.Clear;
               Begin_Func (Ctx.E, Saved_Buf,
                 Link & "func " & To_String (SI.Pir_Name) & "(" & Params & ") -> " & Ret_S & " cc ""internal""");
               Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
               End_Func (Ctx.E, Saved_Buf);
               Ctx.Handlers := Saved_Hdl;
            end;
         end;
         return;
      end if;

      declare
         Saved_Sub     : constant Tree_Maps.Map := Ctx.Generic_Subst;
         Saved_Tag     : constant Unbounded_String := Ctx.Gen_Inst_Tag;
         Saved_Exc     : constant U32_Str_Maps.Map := Ctx.Gen_Local_Exc_Tags;
         Post_Spec_Exc : U32_Str_Maps.Map := Ctx.Gen_Local_Exc_Tags;
         Saved_In_GB   : constant Boolean := Ctx.In_Gen_Body;
         Saved_NGens   : constant U32_Vecs.Vector := Ctx.Gen_Body_Nested_Gens;
         Restore_Vars  : Var_Maps.Map;
         Del_Vars      : U32_Vecs.Vector;
         Restore_Subp  : Subp_Maps.Map;
         Del_Subp      : U32_Vecs.Vector;
         Restore_Tasks : Task_Maps.Map;
         Del_Tasks     : U32_Vecs.Vector;
         Gen_Is_Global : constant Boolean := (Ctx.Cur_Subp_Key = 0) or else Ctx.Has_Stubs;
         FItems    : SEQ_TYPE := (if Is_Void_Tree (Param_S) then MAKE else LIST (Param_S));
         ADecls_N  : constant TREE := D (SM_DECL_S, Unit_K);
         ADecls    : SEQ_TYPE := (if Is_Void_Tree (ADecls_N) then MAKE else LIST (ADecls_N));
         FItem, ADecl : TREE;
         Dummy_Dyn : Boolean;
         -- Helper: LRM 12.3.1/12.3.4/12.3.5 runtime subtype constraint matching for generic formal vs actual
         procedure Emit_Check_Subtypes_Match
           (T1_In, T2_In : TREE;
            Depth        : Natural := 0;
            Is_Formal_Obj : Boolean := False)
         is
            T1 : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, T1_In));
            T2 : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, T2_In));
         begin
            if not Ctx.Enable_Checks or else Is_Void_Tree (T1) or else Is_Void_Tree (T2) or else Depth > 4 then
               return;
            end if;
            if T1 = T2 then
               return;
            end if;
            -- LRM 12.3.1(4): an unconstrained formal IN OUT object accepts any actual subtype (constrained or unconstrained)
            if Is_Formal_Obj and then KIND (T1) in DN_RECORD | DN_ARRAY | DN_ACCESS then
               return;
            end if;
            if KIND (T1) in DN_INTEGER | DN_ENUMERATION and then KIND (T2) in DN_INTEGER | DN_ENUMERATION then
               declare
                  F1, L1, F2, L2 : Unbounded_String;
                  C1  : constant String := Next_Reg (Ctx.E, "gm1");
                  C2  : constant String := Next_Reg (Ctx.E, "gm2");
                  Bad : constant String := Next_Reg (Ctx.E, "gmb");
               begin
                  Emit_Index_Bounds (Ctx, T1, F1, L1);
                  Emit_Index_Bounds (Ctx, T2, F2, L2);
                  Emit_Inst (Ctx.E, C1 & " = cmp_ne.i64 " & To_String (F1) & ", " & To_String (F2));
                  Emit_Inst (Ctx.E, C2 & " = cmp_ne.i64 " & To_String (L1) & ", " & To_String (L2));
                  Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                  Emit_Check_Fail_If (Ctx, Bad);
               end;
            elsif KIND (T1) = DN_CONSTRAINED_ARRAY and then KIND (T2) = DN_CONSTRAINED_ARRAY then
               declare
                  TI1 : constant Type_Info := Get_Type_Info (Ctx, T1);
                  TI2 : constant Type_Info := Get_Type_Info (Ctx, T2);
                  ND  : constant Natural := Natural'Min (TI1.NDims, TI2.NDims);
               begin
                  for D_I in 1 .. ND loop
                     declare
                        F1, L1, F2, L2 : Unbounded_String;
                        C1  : constant String := Next_Reg (Ctx.E, "ga1");
                        C2  : constant String := Next_Reg (Ctx.E, "ga2");
                        Bad : constant String := Next_Reg (Ctx.E, "gab");
                     begin
                        Emit_Index_Bounds (Ctx, TI1.Dims (D_I).Idx_Type, F1, L1);
                        Emit_Index_Bounds (Ctx, TI2.Dims (D_I).Idx_Type, F2, L2);
                        Emit_Inst (Ctx.E, C1 & " = cmp_ne.i64 " & To_String (F1) & ", " & To_String (F2));
                        Emit_Inst (Ctx.E, C2 & " = cmp_ne.i64 " & To_String (L1) & ", " & To_String (L2));
                        Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end loop;
                  Emit_Check_Subtypes_Match (TI1.Elem_Type, TI2.Elem_Type, Depth + 1);
               end;
            elsif KIND (T1) = DN_ARRAY and then KIND (T2) = DN_ARRAY then
               declare
                  IS1 : constant TREE := D (SM_INDEX_S, T1);
                  IS2 : constant TREE := D (SM_INDEX_S, T2);
               begin
                  if not Is_Void_Tree (IS1) and then not Is_Void_Tree (IS2) then
                     declare
                        L1 : SEQ_TYPE := LIST (IS1);
                        L2 : SEQ_TYPE := LIST (IS2);
                        I1, I2 : TREE;
                     begin
                        while not IS_EMPTY (L1) and then not IS_EMPTY (L2) loop
                           POP (L1, I1);
                           POP (L2, I2);
                           Emit_Check_Subtypes_Match (D (SM_TYPE_SPEC, I1), D (SM_TYPE_SPEC, I2), Depth + 1);
                        end loop;
                     end;
                  end if;
               end;
               Emit_Check_Subtypes_Match (D (SM_COMP_TYPE, T1), D (SM_COMP_TYPE, T2), Depth + 1);
            elsif KIND (T1) = DN_FLOAT and then KIND (T2) = DN_FLOAT then
               declare
                  A1 : constant TREE := D (SM_ACCURACY, T1);
                  A2 : constant TREE := D (SM_ACCURACY, T2);
                  R1 : constant TREE := D (SM_RANGE, T1);
                  R2 : constant TREE := D (SM_RANGE, T2);
               begin
                  if not Is_Void_Tree (A1) and then not Is_Void_Tree (A2)
                    and then Has_Static_Val (A1)
                    and then Has_Static_Val (A2)
                    and then Eval_Int64 (A1) /= Eval_Int64 (A2)
                  then
                     Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                     declare
                        Unr : constant String := Next_Label (Ctx.E, "gf_u");
                     begin
                        Emit_Label (Ctx.E, Unr);
                     end;
                     return;
                  end if;
                  if not Is_Void_Tree (R1) and then KIND (R1) = DN_RANGE
                    and then not Is_Void_Tree (R2) and then KIND (R2) = DN_RANGE
                  then
                     declare
                        L1V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, R1), T1);
                        H1V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, R1), T1);
                        L2V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, R2), T2);
                        H2V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, R2), T2);
                        L1  : constant String := Emit_Scalar_Coerce (Ctx, To_String (L1V.Reg), To_String (L1V.Pir_Type), "f64");
                        H1  : constant String := Emit_Scalar_Coerce (Ctx, To_String (H1V.Reg), To_String (H1V.Pir_Type), "f64");
                        L2  : constant String := Emit_Scalar_Coerce (Ctx, To_String (L2V.Reg), To_String (L2V.Pir_Type), "f64");
                        H2  : constant String := Emit_Scalar_Coerce (Ctx, To_String (H2V.Reg), To_String (H2V.Pir_Type), "f64");
                        N1  : constant String := Next_Reg (Ctx.E, "gfn1");
                        N2  : constant String := Next_Reg (Ctx.E, "gfn2");
                        BN  : constant String := Next_Reg (Ctx.E, "gfbn");
                        NB  : constant String := Next_Reg (Ctx.E, "gfnb");
                        C1  : constant String := Next_Reg (Ctx.E, "gf1");
                        C2  : constant String := Next_Reg (Ctx.E, "gf2");
                        Mis : constant String := Next_Reg (Ctx.E, "gfm");
                        Bad : constant String := Next_Reg (Ctx.E, "gfb");
                     begin
                        Emit_Inst (Ctx.E, N1 & " = fcmp_ogt.f64 " & L1 & ", " & H1);
                        Emit_Inst (Ctx.E, N2 & " = fcmp_ogt.f64 " & L2 & ", " & H2);
                        Emit_Inst (Ctx.E, BN & " = and.i8 " & N1 & ", " & N2);
                        Emit_Inst (Ctx.E, NB & " = xor.i8 " & BN & ", 1");
                        Emit_Inst (Ctx.E, C1 & " = fcmp_one.f64 " & L1 & ", " & L2);
                        Emit_Inst (Ctx.E, C2 & " = fcmp_one.f64 " & H1 & ", " & H2);
                        Emit_Inst (Ctx.E, Mis & " = or.i8 " & C1 & ", " & C2);
                        Emit_Inst (Ctx.E, Bad & " = and.i8 " & Mis & ", " & NB);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end if;
               end;
            elsif KIND (T1) = DN_FIXED and then KIND (T2) = DN_FIXED then
               declare
                  D1 : constant IEEE_Float_64 := Fixed_Delta (T1);
                  D2 : constant IEEE_Float_64 := Fixed_Delta (T2);
                  Sm1 : constant IEEE_Float_64 := Fixed_Small (T1);
                  Sm2 : constant IEEE_Float_64 := Fixed_Small (T2);
                  R1 : constant TREE := D (SM_RANGE, T1);
                  R2 : constant TREE := D (SM_RANGE, T2);
               begin
                  if D1 /= D2 or else Sm1 /= Sm2 then
                     Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                     declare
                        Unr : constant String := Next_Label (Ctx.E, "gx_u");
                     begin
                        Emit_Label (Ctx.E, Unr);
                     end;
                     return;
                  end if;
                  if not Is_Void_Tree (R1) and then KIND (R1) = DN_RANGE
                    and then not Is_Void_Tree (R2) and then KIND (R2) = DN_RANGE
                  then
                     declare
                        L1V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, R1), T1);
                        H1V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, R1), T1);
                        L2V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, R2), T2);
                        H2V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, R2), T2);
                        L1  : constant String := Emit_Int_Convert (Ctx, To_String (L1V.Reg), To_String (L1V.Pir_Type), "i64");
                        H1  : constant String := Emit_Int_Convert (Ctx, To_String (H1V.Reg), To_String (H1V.Pir_Type), "i64");
                        L2  : constant String := Emit_Int_Convert (Ctx, To_String (L2V.Reg), To_String (L2V.Pir_Type), "i64");
                        H2  : constant String := Emit_Int_Convert (Ctx, To_String (H2V.Reg), To_String (H2V.Pir_Type), "i64");
                        N1  : constant String := Next_Reg (Ctx.E, "gxn1");
                        N2  : constant String := Next_Reg (Ctx.E, "gxn2");
                        BN  : constant String := Next_Reg (Ctx.E, "gxbn");
                        NB  : constant String := Next_Reg (Ctx.E, "gxnb");
                        C1  : constant String := Next_Reg (Ctx.E, "gx1");
                        C2  : constant String := Next_Reg (Ctx.E, "gx2");
                        Mis : constant String := Next_Reg (Ctx.E, "gxm");
                        Bad : constant String := Next_Reg (Ctx.E, "gxb");
                     begin
                        Emit_Inst (Ctx.E, N1 & " = cmp_gt_s.i64 " & L1 & ", " & H1);
                        Emit_Inst (Ctx.E, N2 & " = cmp_gt_s.i64 " & L2 & ", " & H2);
                        Emit_Inst (Ctx.E, BN & " = and.i8 " & N1 & ", " & N2);
                        Emit_Inst (Ctx.E, NB & " = xor.i8 " & BN & ", 1");
                        Emit_Inst (Ctx.E, C1 & " = cmp_ne.i64 " & L1 & ", " & L2);
                        Emit_Inst (Ctx.E, C2 & " = cmp_ne.i64 " & H1 & ", " & H2);
                        Emit_Inst (Ctx.E, Mis & " = or.i8 " & C1 & ", " & C2);
                        Emit_Inst (Ctx.E, Bad & " = and.i8 " & Mis & ", " & NB);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end if;
               end;
            elsif KIND (T1) = DN_CONSTRAINED_RECORD and then KIND (T2) = DN_CONSTRAINED_RECORD then
               declare
                  S1 : constant TREE := D (SM_NORMALIZED_DSCRMT_S, T1);
                  S2 : constant TREE := D (SM_NORMALIZED_DSCRMT_S, T2);
               begin
                  if not Is_Void_Tree (S1) and then not Is_Void_Tree (S2) then
                     declare
                        L1 : SEQ_TYPE := LIST (S1);
                        L2 : SEQ_TYPE := LIST (S2);
                        E1, E2 : TREE;
                     begin
                        while not IS_EMPTY (L1) and then not IS_EMPTY (L2) loop
                           POP (L1, E1);
                           POP (L2, E2);
                           declare
                              V1  : constant Expr_Val := Emit_Expr (Ctx, E1);
                              V2  : constant Expr_Val := Emit_Expr (Ctx, E2);
                              I1  : constant String := Emit_Int_Convert (Ctx, To_String (V1.Reg), To_String (V1.Pir_Type), "i64");
                              I2  : constant String := Emit_Int_Convert (Ctx, To_String (V2.Reg), To_String (V2.Pir_Type), "i64");
                              Bad : constant String := Next_Reg (Ctx.E, "grb");
                           begin
                              Emit_Inst (Ctx.E, Bad & " = cmp_ne.i64 " & I1 & ", " & I2);
                              Emit_Check_Fail_If (Ctx, Bad);
                           end;
                        end loop;
                     end;
                  end if;
               end;
            elsif KIND (T1) in DN_ACCESS | DN_CONSTRAINED_ACCESS
              and then KIND (T2) in DN_ACCESS | DN_CONSTRAINED_ACCESS
            then
               Emit_Check_Subtypes_Match (D (SM_DESIG_TYPE, T1), D (SM_DESIG_TYPE, T2), Depth + 1);
            elsif (KIND (T1) = DN_CONSTRAINED_ARRAY and then KIND (T2) = DN_ARRAY)
              or else (KIND (T1) = DN_ARRAY and then KIND (T2) = DN_CONSTRAINED_ARRAY)
              or else (KIND (T1) = DN_CONSTRAINED_RECORD and then KIND (T2) = DN_RECORD)
              or else (KIND (T1) = DN_RECORD and then KIND (T2) = DN_CONSTRAINED_RECORD)
            then
               Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
               declare
                  Unr : constant String := Next_Label (Ctx.E, "gm_u");
               begin
                  Emit_Label (Ctx.E, Unr);
               end;
            end if;
         end Emit_Check_Subtypes_Match;
      begin
         Ctx.Gen_Inst_Tag := Saved_Tag & "_gi" & Tree_Key_Str (Inst_Id);
         Ctx.Gen_Body_Nested_Gens.Clear;
         -- If this generic was declared inside an instantiated outer generic (and we are NOT still
         -- actively emitting that same outer generic body right now), restore outer formal bindings,
         -- outer body exception tags, and outer body vars/subps (LRM 12.3(17), 11.1(5)).
         declare
            In_Cur_Enclosing_Gen : Boolean := False;
         begin
            for NGK of Saved_NGens loop
               if NGK = Gen_Key then
                  In_Cur_Enclosing_Gen := True;
                  exit;
               end if;
            end loop;
         if not In_Cur_Enclosing_Gen and then Ctx.Generic_Env.Contains (Gen_Key) then
            declare
               Env : constant Tree_Maps.Map := Ctx.Generic_Env.Element (Gen_Key);
               Cur : Tree_Maps.Cursor := Env.First;
            begin
               while Tree_Maps.Has_Element (Cur) loop
                  Ctx.Generic_Subst.Include (Tree_Maps.Key (Cur), Tree_Maps.Element (Cur));
                  Tree_Maps.Next (Cur);
               end loop;
            end;
         end if;
         if not In_Cur_Enclosing_Gen and then Ctx.Gen_Env_Exc.Contains (Gen_Key) then
            declare
               EEnv : constant U32_Str_Maps.Map := Ctx.Gen_Env_Exc.Element (Gen_Key);
               Cur  : U32_Str_Maps.Cursor := EEnv.First;
            begin
               while U32_Str_Maps.Has_Element (Cur) loop
                  Ctx.Gen_Local_Exc_Tags.Include (U32_Str_Maps.Key (Cur), U32_Str_Maps.Element (Cur));
                  U32_Str_Maps.Next (Cur);
               end loop;
            end;
         end if;
         if not In_Cur_Enclosing_Gen and then Ctx.Gen_Env_Vars.Contains (Gen_Key) then
            declare
               VEnv : constant Var_Maps.Map := Ctx.Gen_Env_Vars.Element (Gen_Key);
               Cur  : Var_Maps.Cursor := VEnv.First;
            begin
               while Var_Maps.Has_Element (Cur) loop
                  declare
                     VK : constant Unsigned_32 := Var_Maps.Key (Cur);
                  begin
                     if Ctx.Vars.Contains (VK) then
                        Restore_Vars.Include (VK, Ctx.Vars.Element (VK));
                     else
                        Del_Vars.Append (VK);
                     end if;
                     Ctx.Vars.Include (VK, Var_Maps.Element (Cur));
                  end;
                  Var_Maps.Next (Cur);
               end loop;
            end;
         end if;
         if not In_Cur_Enclosing_Gen and then Ctx.Gen_Env_Subps.Contains (Gen_Key) then
            declare
               SEnv : constant Subp_Maps.Map := Ctx.Gen_Env_Subps.Element (Gen_Key);
               Cur  : Subp_Maps.Cursor := SEnv.First;
            begin
               while Subp_Maps.Has_Element (Cur) loop
                  declare
                     SK : constant Unsigned_32 := Subp_Maps.Key (Cur);
                  begin
                     if Ctx.Subps.Contains (SK) then
                        Restore_Subp.Include (SK, Ctx.Subps.Element (SK));
                     else
                        Del_Subp.Append (SK);
                     end if;
                     Ctx.Subps.Include (SK, Subp_Maps.Element (Cur));
                  end;
                  Subp_Maps.Next (Cur);
               end loop;
            end;
         end if;
         if not In_Cur_Enclosing_Gen and then Ctx.Gen_Env_Tasks.Contains (Gen_Key) then
            declare
               TEnv : constant Task_Maps.Map := Ctx.Gen_Env_Tasks.Element (Gen_Key);
               Cur  : Task_Maps.Cursor := TEnv.First;
            begin
               while Task_Maps.Has_Element (Cur) loop
                  declare
                     TK_E : constant Unsigned_32 := Task_Maps.Key (Cur);
                  begin
                     if Ctx.Tasks.Contains (TK_E) then
                        Restore_Tasks.Include (TK_E, Ctx.Tasks.Element (TK_E));
                     else
                        Del_Tasks.Append (TK_E);
                     end if;
                     Ctx.Tasks.Include (TK_E, Task_Maps.Element (Cur));
                  end;
                  Task_Maps.Next (Cur);
               end loop;
            end;
         end if;
         end;
         -- Map formal generic parameters -> actual bindings & verify LRM 12.3 subtype constraint matching.
         -- Per LRM 12.3(16) (cc3123b): evaluate explicit generic IN actuals BEFORE defaulted IN formals!
         declare
            Exp_Glob : constant Boolean := (Export_All and then Ctx.Cur_Subp_Key = 0);
            Assoc_S  : constant TREE := D (AS_GENERAL_ASSOC_S, Unit_K);
            Pos_Cnt  : Natural := 0;
            Named_Arr : array (1 .. 64) of Unbounded_String;
            Named_Cnt : Natural := 0;

            function Is_Formal_Explicit (F_Id : TREE; Param_Idx : Natural) return Boolean is
               FN : constant String := Sym_Name (F_Id);
            begin
               if Param_Idx <= Pos_Cnt then
                  return True;
               end if;
               for NI in 1 .. Named_Cnt loop
                  if To_String (Named_Arr (NI)) = FN then
                     return True;
                  end if;
               end loop;
               return False;
            end Is_Formal_Explicit;

            type Def_In_Item is record
               F_Id  : TREE;
               ADecl : TREE;
            end record;
            Def_Ins  : array (1 .. 64) of Def_In_Item;
            Def_Cnt  : Natural := 0;
            type Deferred_Match_Pair is record
               T1    : TREE;
               T2    : TREE;
               Sub   : Tree_Maps.Map;
            end record;
            package Def_Match_Vecs is new Ada.Containers.Vectors (Natural, Deferred_Match_Pair);
            Def_Matches : Def_Match_Vecs.Vector;
            P_Idx    : Natural := 0;
         begin
            if not Is_Void_Tree (Assoc_S) then
               declare
                  AL : SEQ_TYPE := LIST (Assoc_S);
                  AI : TREE;
                  Saw_Named : Boolean := False;
               begin
                  while not IS_EMPTY (AL) loop
                     POP (AL, AI);
                     if KIND (AI) = DN_ASSOC then
                        Saw_Named := True;
                        declare
                           Ch : constant TREE := D (AS_USED_NAME, AI);
                        begin
                           if not Is_Void_Tree (Ch) and then Named_Cnt < Named_Arr'Last then
                              Named_Cnt := Named_Cnt + 1;
                              Named_Arr (Named_Cnt) := To_Unbounded_String (Sym_Name (Ch));
                           end if;
                        end;
                     elsif not Saw_Named then
                        Pos_Cnt := Pos_Cnt + 1;
                     end if;
                  end loop;
               end;
            end if;

            while not IS_EMPTY (FItems) and then not IS_EMPTY (ADecls) loop
               POP (FItems, FItem);
               if KIND (FItem) = DN_IN then
                  declare
                     FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FItem));
                     F_Id : TREE;
                  begin
                     while not IS_EMPTY (FNS) and then not IS_EMPTY (ADecls) loop
                        POP (FNS, F_Id);
                        POP (ADecls, ADecl);
                        P_Idx := P_Idx + 1;
                        if KIND (ADecl) = DN_CONSTANT_DECL then
                           declare
                              A_Id : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, ADecl)));
                           begin
                              if Is_Formal_Explicit (F_Id, P_Idx) or else Def_Cnt >= Def_Ins'Last then
                                 Emit_Decl_Items (Ctx, CONST_VOID, Is_Global => Gen_Is_Global, Export_All => Exp_Glob, Has_Dyn => Dummy_Dyn, Only_Item => ADecl);
                                 Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (F_Id)), Canonical_Def (A_Id));
                              else
                                 Def_Cnt := Def_Cnt + 1;
                                 Def_Ins (Def_Cnt) := (F_Id => F_Id, ADecl => ADecl);
                              end if;
                           end;
                        end if;
                     end loop;
                  end;
               elsif KIND (FItem) = DN_IN_OUT then
                  declare
                     FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FItem));
                     F_Id : TREE;
                  begin
                     while not IS_EMPTY (FNS) and then not IS_EMPTY (ADecls) loop
                        POP (FNS, F_Id);
                        POP (ADecls, ADecl);
                        P_Idx := P_Idx + 1;
                        if KIND (ADecl) = DN_RENAMES_OBJ_DECL then
                           Emit_Decl_Items (Ctx, CONST_VOID, Is_Global => Gen_Is_Global, Export_All => Exp_Glob, Has_Dyn => Dummy_Dyn, Only_Item => ADecl);
                           declare
                              A_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, ADecl));
                           begin
                              -- Per LRM 12.3.1(3) & AI-00051: generic formal IN OUT object adopts the actual's subtype
                              Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (F_Id)), A_Id);
                           end;
                        end if;
                     end loop;
                  end;
               else
                  POP (ADecls, ADecl);
                  P_Idx := P_Idx + 1;
                  Emit_Decl_Items (Ctx, CONST_VOID, Is_Global => Gen_Is_Global, Export_All => Exp_Glob, Has_Dyn => Dummy_Dyn, Only_Item => ADecl);
                  if KIND (FItem) = DN_TYPE_DECL and then KIND (ADecl) = DN_SUBTYPE_DECL then
                     declare
                        F_Tid : constant TREE := D (AS_SOURCE_NAME, FItem);
                        F_Ts  : constant TREE := D (SM_TYPE_SPEC, F_Tid);
                        A_Sid : constant TREE := D (AS_SOURCE_NAME, ADecl);
                        A_Ts  : constant TREE := D (SM_TYPE_SPEC, A_Sid);
                     begin
                        -- Defer subtype match checks until Pass B binds any defaulted IN formals used in formal type constraints!
                        if not Is_Void_Tree (F_Ts) and then not Is_Void_Tree (A_Ts) then
                           if KIND (F_Ts) in DN_ARRAY | DN_CONSTRAINED_ARRAY | DN_ACCESS | DN_CONSTRAINED_ACCESS then
                              if Def_Cnt > 0 then
                                 Def_Matches.Append ((T1 => F_Ts, T2 => A_Ts, Sub => Ctx.Generic_Subst));
                              else
                                 Emit_Check_Subtypes_Match (F_Ts, A_Ts);
                              end if;
                           elsif KIND (F_Ts) in DN_PRIVATE | DN_L_PRIVATE then
                              declare
                                 F_DS : constant TREE := D (SM_DISCRIMINANT_S, F_Ts);
                                 A_BT : constant TREE := Base_Type_Spec (Resolve_Type_Spec (A_Ts));
                                 A_DS : constant TREE :=
                                   (if not Is_Void_Tree (A_BT) and then KIND (A_BT) in DN_RECORD | DN_PRIVATE | DN_L_PRIVATE
                                    then D (SM_DISCRIMINANT_S, A_BT) else CONST_VOID);
                              begin
                                 if not Is_Void_Tree (F_DS) and then not IS_EMPTY (LIST (F_DS))
                                   and then not Is_Void_Tree (A_DS) and then not IS_EMPTY (LIST (A_DS))
                                 then
                                    declare
                                       FL : SEQ_TYPE := LIST (F_DS);
                                       AL : SEQ_TYPE := LIST (A_DS);
                                       FD, AD : TREE;
                                       FNS, ANS : SEQ_TYPE := MAKE;
                                       FID, AID : TREE;
                                    begin
                                       loop
                                          if IS_EMPTY (FNS) then
                                             exit when IS_EMPTY (FL);
                                             POP (FL, FD);
                                             FNS := LIST (D (AS_SOURCE_NAME_S, FD));
                                          end if;
                                          if IS_EMPTY (ANS) then
                                             exit when IS_EMPTY (AL);
                                             POP (AL, AD);
                                             ANS := LIST (D (AS_SOURCE_NAME_S, AD));
                                          end if;
                                          POP (FNS, FID);
                                          POP (ANS, AID);
                                          if Def_Cnt > 0 then
                                             Def_Matches.Append ((T1 => D (SM_OBJ_TYPE, FID), T2 => D (SM_OBJ_TYPE, AID), Sub => Ctx.Generic_Subst));
                                          else
                                             Emit_Check_Subtypes_Match (D (SM_OBJ_TYPE, FID), D (SM_OBJ_TYPE, AID));
                                          end if;
                                          Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (FID)), Canonical_Def (AID));
                                          Ctx.Generic_Subst.Include (Tree_Key (FID), Canonical_Def (AID));
                                       end loop;
                                    end;
                                 end if;
                              end;
                           end if;
                        end if;
                        Ctx.Generic_Subst.Include (Tree_Key (F_Tid), A_Sid);
                        if not Is_Void_Tree (F_Ts) and then not Is_Void_Tree (A_Ts) then
                           Ctx.Generic_Subst.Include (Tree_Key (F_Ts), A_Ts);
                        end if;
                     end;
                  elsif KIND (FItem) = DN_SUBPROG_ENTRY_DECL and then KIND (ADecl) = DN_SUBPROG_ENTRY_DECL then
                     declare
                        F_Sid  : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, FItem));
                        A_Unit : constant TREE := D (AS_UNIT_KIND, ADecl);
                     begin
                        if not Is_Void_Tree (A_Unit) and then KIND (A_Unit) = DN_RENAMES_UNIT then
                           declare
                              RN : constant TREE := D (AS_NAME, A_Unit);
                           begin
                              declare
                                 A_Sid : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, ADecl));
                                 A_Sub : constant TREE :=
                                   (if not Is_Void_Tree (RN) and then KIND (RN) /= DN_ATTRIBUTE
                                    then Canonical_Def (Get_Name_Defn (RN)) else CONST_VOID);
                              begin
                                 if (not Is_Void_Tree (RN) and then KIND (RN) = DN_ATTRIBUTE)
                                   or else (not Is_Void_Tree (A_Sid) and then Ctx.Renamed_Entries.Contains (Tree_Key (A_Sid)))
                                   or else (not Is_Void_Tree (A_Sub) and then KIND (A_Sub) = DN_ENTRY_ID)
                                 then
                                    -- Attribute default or task entry / renamed-entry actual: map to ADecl's ID
                                    Ctx.Generic_Subst.Include (Tree_Key (F_Sid), A_Sid);
                                 else
                                    Ctx.Generic_Subst.Include (Tree_Key (F_Sid), A_Sub);
                                 end if;
                              end;
                           end;
                        end if;
                     end;
                  end if;
               end if;
            end loop;

            -- Emit remaining trailing ADecls (if any)
            while not IS_EMPTY (ADecls) loop
               POP (ADecls, ADecl);
               Emit_Decl_Items (Ctx, CONST_VOID, Is_Global => Gen_Is_Global, Export_All => Exp_Glob, Has_Dyn => Dummy_Dyn, Only_Item => ADecl);
            end loop;

            -- Pass B: emit defaulted generic IN formal parameters in declaration order!
            for DI in 1 .. Def_Cnt loop
               Emit_Decl_Items (Ctx, CONST_VOID, Is_Global => Gen_Is_Global, Export_All => Exp_Glob, Has_Dyn => Dummy_Dyn, Only_Item => Def_Ins (DI).ADecl);
               declare
                  A_Id : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, Def_Ins (DI).ADecl)));
               begin
                  Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (Def_Ins (DI).F_Id)), Canonical_Def (A_Id));
               end;
            end loop;

            -- Run deferred subtype match checks now that defaulted IN formals are bound (LRM 12.3(16)/12.3.4(4))
            if not Def_Matches.Is_Empty then
               declare
                  Full_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
               begin
                  for DM of Def_Matches loop
                     Ctx.Generic_Subst := DM.Sub;
                     for DI in 1 .. Def_Cnt loop
                        declare
                           A_Id : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, Def_Ins (DI).ADecl)));
                        begin
                           Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (Def_Ins (DI).F_Id)), Canonical_Def (A_Id));
                        end;
                     end loop;
                     Emit_Check_Subtypes_Match (DM.T1, DM.T2);
                  end loop;
                  Ctx.Generic_Subst := Full_Sub;
               end;
            end if;
         end;

         -- For generic package instantiations, emit instantiated spec declarations AFTER ADecls_N
         declare
            Saved_Pkg_G : constant Integer_64 := Ctx.Cur_Task_Group_Id;
            Pkg_Gid     : Integer_64 := 0;
            procedure Restore_Overlays is
            begin
               for VK of Del_Vars loop
                  if Ctx.Vars.Contains (VK) then Ctx.Vars.Delete (VK); end if;
               end loop;
               for C in Restore_Vars.Iterate loop
                  Ctx.Vars.Include (Var_Maps.Key (C), Var_Maps.Element (C));
               end loop;
               for SK of Del_Subp loop
                  if Ctx.Subps.Contains (SK) then Ctx.Subps.Delete (SK); end if;
               end loop;
               for C in Restore_Subp.Iterate loop
                  Ctx.Subps.Include (Subp_Maps.Key (C), Subp_Maps.Element (C));
               end loop;
               for TK_R of Del_Tasks loop
                  if Ctx.Tasks.Contains (TK_R) then Ctx.Tasks.Delete (TK_R); end if;
               end loop;
               for C in Restore_Tasks.Iterate loop
                  Ctx.Tasks.Include (Task_Maps.Key (C), Task_Maps.Element (C));
               end loop;
               Ctx.In_Gen_Body          := Saved_In_GB;
               Ctx.Gen_Body_Nested_Gens := Saved_NGens;
               Ctx.Generic_Subst        := Saved_Sub;
               Ctx.Gen_Inst_Tag         := Saved_Tag;
               Ctx.Gen_Local_Exc_Tags   := Post_Spec_Exc;
            end Restore_Overlays;
         begin
         if KIND (Inst_Decl) = DN_PACKAGE_DECL then
            declare
               ISpec : constant TREE := D (SM_SPEC, Inst_Id);
            begin
               if not Is_Void_Tree (GBody) then
                  Pkg_Gid := Ctx.Next_Task_Group_Id;
                  Ctx.Next_Task_Group_Id := Ctx.Next_Task_Group_Id + 1;
                  Ctx.Cur_Task_Group_Id := Pkg_Gid;
               end if;
               if not Is_Void_Tree (ISpec) and then KIND (ISpec) = DN_PACKAGE_SPEC then
                  Emit_Decl_Items (Ctx, D (AS_DECL_S1, ISpec), Is_Global => Gen_Is_Global, Export_All => Export_All, Has_Dyn => Dummy_Dyn);
                  Emit_Decl_Items (Ctx, D (AS_DECL_S2, ISpec), Is_Global => Gen_Is_Global, Export_All => Export_All, Has_Dyn => Dummy_Dyn);
                  -- P3-5.9: Preserve any spec-level exception tags declared inside ISpec for the enclosing generic body!
                  if Saved_In_GB then
                     for C in Ctx.Gen_Local_Exc_Tags.Iterate loop
                        if not Saved_Exc.Contains (U32_Str_Maps.Key (C)) then
                           Post_Spec_Exc.Include (U32_Str_Maps.Key (C), U32_Str_Maps.Element (C));
                        end if;
                     end loop;
                  end if;
               end if;
            end;
            if Is_Void_Tree (GBody) then
               Ctx.Cur_Task_Group_Id := Saved_Pkg_G;
               Restore_Overlays;
               return;
            end if;
         end if;

         -- Isolate per-instantiation Saved_Type_Ranges / Saved_Type_Discs / Saved_Expr_Vals across generic bodies (LRM 12.3(17))
         declare
            Saved_Ranges     : constant Saved_Range_Maps.Map := Ctx.Saved_Type_Ranges;
            Saved_Var_Rngs   : constant Saved_Range_Maps.Map := Ctx.Saved_Var_Ranges;
            Saved_Discs      : constant Saved_Disc_Maps.Map  := Ctx.Saved_Type_Discs;
            Saved_Exprs      : constant Saved_Expr_Maps.Map  := Ctx.Saved_Expr_Vals;
         begin
         Ctx.In_Gen_Body := True;

         -- Case 1: Generic subprogram instantiation
         if KIND (GBody) = DN_SUBPROGRAM_BODY then
            declare
               Inst_Spec : constant TREE := D (SM_SPEC, Inst_Id);
               Gen_Spec  : constant TREE := D (SM_SPEC, Gen_Id);
               Body_Spec : constant TREE := D (AS_HEADER, GBody);

               procedure Map_Param_Lists (From_Spec, To_Spec : TREE) is
                  FL : SEQ_TYPE;
                  TL : SEQ_TYPE;
                  FP, TP : TREE;
               begin
                  if Is_Void_Tree (From_Spec) or else Is_Void_Tree (To_Spec) then
                     return;
                  end if;
                  if Is_Void_Tree (D (AS_PARAM_S, From_Spec)) or else Is_Void_Tree (D (AS_PARAM_S, To_Spec)) then
                     return;
                  end if;
                  FL := LIST (D (AS_PARAM_S, From_Spec));
                  TL := LIST (D (AS_PARAM_S, To_Spec));
                  while not IS_EMPTY (FL) and then not IS_EMPTY (TL) loop
                     POP (FL, FP); POP (TL, TP);
                     declare
                        FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FP));
                        TNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, TP));
                        FID, TID : TREE;
                     begin
                        while not IS_EMPTY (FNS) and then not IS_EMPTY (TNS) loop
                           POP (FNS, FID); POP (TNS, TID);
                           Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (FID)), Canonical_Def (TID));
                           Ctx.Generic_Subst.Include (Tree_Key (FID), Canonical_Def (TID));
                        end loop;
                     end;
                  end loop;
               end Map_Param_Lists;
            begin
               Map_Param_Lists (Gen_Spec, Inst_Spec);
               Map_Param_Lists (Body_Spec, Inst_Spec);
               -- LRM 12.3 (c58004g, c83030a/c): map generic subprogram ID -> Inst_Id for recursive self-calls!
               Ctx.Generic_Subst.Include (Tree_Key (Gen_Id), Inst_Id);
               Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (Gen_Id)), Inst_Id);
               if not Is_Void_Tree (D (AS_SOURCE_NAME, GBody)) then
                  Ctx.Generic_Subst.Include (Tree_Key (D (AS_SOURCE_NAME, GBody)), Inst_Id);
                  Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (D (AS_SOURCE_NAME, GBody))), Inst_Id);
               end if;
               -- Propagate Needs_SL from generic template or enclosing frame to Inst_Id
               if Ctx.Cur_Subp_Key /= 0 then
                  declare
                     IK      : constant Unsigned_32 := Tree_Key (Inst_Id);
                     Need_SL : Boolean := False;
                  begin
                     if Ctx.Subps.Contains (Gen_Key) and then Ctx.Subps.Element (Gen_Key).Needs_SL then
                        Need_SL := True;
                     elsif Ctx.Subps.Contains (Ctx.Cur_Subp_Key)
                       and then (Ctx.Subps.Element (Ctx.Cur_Subp_Key).Has_Frame
                                 or else Ctx.Subps.Element (Ctx.Cur_Subp_Key).Needs_SL)
                     then
                        Need_SL := True;
                     end if;
                     if Need_SL then
                        Pre_Register_Subp (Ctx, Inst_Id, Inst_Spec, Ctx.Cur_Subp_Key);
                        declare
                           ISI : Subp_Info := Ctx.Subps.Element (IK);
                        begin
                           ISI.Needs_SL := True;
                           Ctx.Subps.Replace (IK, ISI);
                        end;
                     end if;
                  end;
               end if;
               if not Is_Void_Tree (D (AS_BODY, GBody)) and then KIND (D (AS_BODY, GBody)) = DN_STUB then
                  declare
                     Sub_G : constant TREE := Find_Gen_Subunit_Body (Ctx, GBody);
                  begin
                     if not Is_Void_Tree (Sub_G) then
                        GBody := Sub_G;
                     else
                        Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "generic subunit body must be in same file as spec");
                        raise Program_Error;
                     end if;
                  end;
               end if;
               Emit_Subprogram_Body (Ctx, GBody,
                 Override_Id   => Inst_Id,
                 Override_Spec => Inst_Spec,
                 Is_Exported   => Export_All);
            end;

         -- Case 2: Generic package instantiation!
         elsif KIND (GBody) = DN_PACKAGE_BODY or else KIND (GBody) = DN_BLOCK_BODY then
            declare
               Gen_Spec  : constant TREE := D (SM_SPEC, Gen_Id);
               Inst_Spec : constant TREE := D (SM_SPEC, Inst_Id);
               Bdy_Block : TREE :=
                 (if KIND (GBody) = DN_PACKAGE_BODY then D (AS_BODY, GBody) else GBody);
            begin
               if not Is_Void_Tree (Bdy_Block) and then KIND (Bdy_Block) = DN_STUB then
                  declare
                     Sub_G : constant TREE := Find_Gen_Subunit_Body (Ctx, GBody);
                  begin
                     if not Is_Void_Tree (Sub_G) then
                        Bdy_Block := D (AS_BODY, Sub_G);
                     else
                        Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "generic subunit body must be in same file as spec");
                        raise Program_Error;
                     end if;
                  end;
               end if;
               Ctx.Generic_Subst.Include (Tree_Key (Gen_Id), Inst_Id);
               if not Is_Void_Tree (Gen_Spec) and then KIND (Gen_Spec) = DN_PACKAGE_SPEC
                 and then not Is_Void_Tree (Inst_Spec) and then KIND (Inst_Spec) = DN_PACKAGE_SPEC
               then
                  Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, Gen_Spec), D (AS_DECL_S1, Inst_Spec));
                  Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, Gen_Spec), D (AS_DECL_S2, Inst_Spec));
               end if;
               -- Bind per-instantiation parent frame capture slots for local generic package body variables
               if not Gen_Is_Global and then Ctx.Cur_Subp_Key /= 0 then
                  declare
                     IK : constant Unsigned_32 := Tree_Key (Inst_Id);
                  begin
                     for C in Ctx.Gen_Cap_Slots.Iterate loop
                        declare
                           PK   : constant Unsigned_64 := U64_Cap_Maps.Key (C);
                           CI   : constant Unsigned_32 := Unsigned_32 (Shift_Right (PK, 32) and 16#FFFF_FFFF#);
                           VK   : constant Unsigned_32 := Unsigned_32 (PK and 16#FFFF_FFFF#);
                           Pair : constant Cap_Slot_Pair := U64_Cap_Maps.Element (C);
                        begin
                           if CI = IK
                             and then not (Ctx.Gen_Env_Vars.Contains (Gen_Key) and then Ctx.Gen_Env_Vars.Element (Gen_Key).Contains (VK))
                           then
                              declare
                                 V : Var_Info;
                              begin
                                 V.Owner_Subp        := Ctx.Cur_Subp_Key;
                                 V.Captured_Slot     := Pair.Cap_Slot;
                                 V.Con_Captured_Slot := Pair.Con_Cap_Slot;
                                 Ctx.Vars.Include (VK, V);
                              end;
                           end if;
                        end;
                     end loop;
                  end;
               end if;
               if not Is_Void_Tree (Bdy_Block) and then KIND (Bdy_Block) = DN_BLOCK_BODY then
                  declare
                     Sub_Dyn : Boolean;
                     Alt_S   : constant TREE := D (AS_ALTERNATIVE_S, Bdy_Block);
                     Has_Hdl : constant Boolean := not Is_Void_Tree (Alt_S) and then not IS_EMPTY (LIST (Alt_S));
                     Hdl_Lbl, After_Lbl : Unbounded_String;
                  begin
                     Emit_Decl_Items (Ctx, D (AS_ITEM_S, Bdy_Block),
                       Is_Global  => Gen_Is_Global,
                       Export_All => Export_All,
                       Has_Dyn    => Sub_Dyn);
                     -- Snapshot outer generic body Exc_Tags, Vars, and Subps for any nested generics declared/bodied inside this generic
                     if not Ctx.Gen_Body_Nested_Gens.Is_Empty then
                        declare
                           Cur_Tag    : constant String := To_String (Ctx.Gen_Inst_Tag);
                           Body_Vars  : Var_Maps.Map;
                           Body_Subp  : Subp_Maps.Map;
                           Body_Tasks : Task_Maps.Map;
                        begin
                           for C in Ctx.Vars.Iterate loop
                              declare
                                 VK : constant Unsigned_32 := Var_Maps.Key (C);
                                 VI : constant Var_Info := Var_Maps.Element (C);
                                 Cap_K : constant Unsigned_64 :=
                                   Shift_Left (Unsigned_64 (Tree_Key (Inst_Id)), 32) or Unsigned_64 (VK);
                              begin
                                 if (Cur_Tag'Length > 0 and then Ada.Strings.Fixed.Index (To_String (VI.Pir_Name), Cur_Tag) > 0)
                                   or else (Cur_Tag'Length > 0 and then Ada.Strings.Fixed.Index (To_String (VI.Desc_Global), Cur_Tag) > 0)
                                   or else (not Gen_Is_Global and then Ctx.Gen_Cap_Slots.Contains (Cap_K))
                                 then
                                    Body_Vars.Include (VK, VI);
                                 end if;
                              end;
                           end loop;
                           for C in Ctx.Subps.Iterate loop
                              declare
                                 SI_E : constant Subp_Info := Subp_Maps.Element (C);
                              begin
                                 if Cur_Tag'Length > 0
                                   and then Ada.Strings.Fixed.Index (To_String (SI_E.Pir_Name), Cur_Tag) > 0
                                   and then Ada.Strings.Fixed.Index (To_String (SI_E.Pir_Name), Cur_Tag & "_gi") = 0
                                 then
                                    Body_Subp.Include (Subp_Maps.Key (C), SI_E);
                                 end if;
                              end;
                           end loop;
                           for C in Ctx.Tasks.Iterate loop
                              declare
                                 TI_E : constant Task_Info := Task_Maps.Element (C);
                              begin
                                 if Cur_Tag'Length > 0 and then
                                   (To_String (TI_E.Inst_Tag) = Cur_Tag
                                    or else Ada.Strings.Fixed.Index (To_String (TI_E.Body_Fn), Cur_Tag) > 0)
                                 then
                                    Body_Tasks.Include (Task_Maps.Key (C), TI_E);
                                 end if;
                              end;
                           end loop;
                           for NGK of Ctx.Gen_Body_Nested_Gens loop
                              if not Ctx.Gen_Local_Exc_Tags.Is_Empty then
                                 Ctx.Gen_Env_Exc.Include (NGK, Ctx.Gen_Local_Exc_Tags);
                              end if;
                              if not Body_Vars.Is_Empty then
                                 Ctx.Gen_Env_Vars.Include (NGK, Body_Vars);
                              end if;
                              if not Body_Subp.Is_Empty then
                                 Ctx.Gen_Env_Subps.Include (NGK, Body_Subp);
                              end if;
                              if not Body_Tasks.Is_Empty then
                                 Ctx.Gen_Env_Tasks.Include (NGK, Body_Tasks);
                              end if;
                           end loop;
                        end;
                     end if;
                     if Pkg_Gid /= 0 then
                        Ctx.Cur_Task_Group_Id := Saved_Pkg_G;
                     end if;
                     if Has_Hdl then
                        Hdl_Lbl   := To_Unbounded_String (Next_Label (Ctx.E, "gexc_h"));
                        After_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "gexc_end"));
                        declare
                           H_Mark : Unbounded_String := Null_Unbounded_String;
                           H_SP   : constant String := Next_Reg (Ctx.E, "ghsp");
                        begin
                           Emit_Inst (Ctx.E, H_SP & " = stacksave");
                           if not Ctx.Cur_Ret_Uncon and then Tree_Uses_SS (Ctx, D (AS_STM_S, Bdy_Block)) then
                              Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", H_Mark, Nounwind => True);
                           end if;
                           Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                                 Current_Exn   => Null_Unbounded_String,
                                                 In_Handler    => False,
                                                 SS_Mark       => H_Mark,
                                                 SP_Mark       => To_Unbounded_String (H_SP)));
                        end;
                     end if;
                     if Pkg_Gid /= 0 then
                        declare
                           Mst_G   : Unbounded_String := Ctx.Cur_Master_Reg;
                           Dummy_G : Unbounded_String;
                        begin
                           if Length (Mst_G) = 0 then
                              Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", Mst_G, Nounwind => True);
                           end if;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_group",
                             To_String (Mst_G) & ", " & Img (Long_Long_Integer (Pkg_Gid)), "", Dummy_G);
                        end;
                     end if;
                     Scan_Label_Cleanup_Depths (Ctx, D (AS_STM_S, Bdy_Block), Cleanup_Depth (Ctx));
                     Scan_Label_Cleanup_Depths (Ctx, Alt_S, Cleanup_Depth (Ctx));
                     Emit_Stm_List (Ctx, D (AS_STM_S, Bdy_Block));
                     if Has_Hdl then
                        if not Is_Terminated (Ctx.E) then
                           Emit_Term (Ctx.E, "jmp " & To_String (After_Lbl));
                        end if;
                        Emit_Exception_Handlers (Ctx, Alt_S, To_String (Hdl_Lbl), To_String (After_Lbl));
                        Ctx.Handlers.Delete_Last;
                        Emit_Label (Ctx.E, To_String (After_Lbl));
                     end if;
                  end;
               end if;
            end;
         end if;

         Ctx.Saved_Type_Ranges := Saved_Ranges;
         Ctx.Saved_Var_Ranges  := Saved_Var_Rngs;
         Ctx.Saved_Type_Discs  := Saved_Discs;
         Ctx.Saved_Expr_Vals   := Saved_Exprs;
         end;

         Ctx.Cur_Task_Group_Id := Saved_Pkg_G;
         Restore_Overlays;
         end;
      end;
   end Emit_Generic_Instantiation;

end Cg_Stmt.Generics;
