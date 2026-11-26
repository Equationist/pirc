-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with Ada.Directories;
with Ada.Strings.Fixed;
with Ada.Strings.Maps.Constants;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Ada.Text_IO;
with Interfaces; use Interfaces;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr; use Cg_Expr;
with Lib_Binder;

package body Cg_Stmt is

   -- Forward declarations
   procedure Pre_Register_Subp
     (Ctx         : in out Context;
      Subp_Id     : TREE;
      Spec_Node   : TREE;
      Parent_Subp : Unsigned_32;
      Is_Subunit  : Boolean := False);

   procedure Emit_Subprogram_Body
     (Ctx           : in out Context;
      Body_Node     : TREE;
      Override_Id   : TREE := CONST_VOID;
      Override_Spec : TREE := CONST_VOID;
      Is_Exported   : Boolean := False;
      Is_Subunit    : Boolean := False);

   procedure Emit_Task_Body
     (Ctx       : in out Context;
      Body_Node : TREE);

   procedure Emit_Decl_Items
     (Ctx        : in out Context;
      Item_S     : TREE;
      Is_Global  : Boolean;
      Export_All : Boolean;
      Has_Dyn    : out Boolean;
      Only_Item  : TREE := CONST_VOID);

   procedure Emit_Block_Body
     (Ctx         : in out Context;
      Block_Node  : TREE;
      Is_Subp     : Boolean);

   procedure Emit_Stm_List (Ctx : in out Context; Stm_S : TREE);
   procedure Emit_Stm (Ctx : in out Context; Stm : TREE);
   procedure Emit_Exception_Handlers
     (Ctx        : in out Context;
      Alt_S      : TREE;
      Hdl_Label  : String;
      After_Lbl  : String);
   function Type_Contains_Task (Ctx : in out Context; Raw_T : TREE; Depth : Natural := 0) return Boolean;
   function Item_List_Has_Tasks (Ctx : in out Context; Item_S : TREE) return Boolean;
   procedure Scan_Label_Master_Depths
     (Ctx      : in out Context;
      Node     : TREE;
      Depth    : Natural;
      SS_Depth : Natural := 0;
      SP_Depth : Natural := 0);

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

   type Cap_Pair is record
      Var_Key  : Unsigned_32 := 0;
      Inst_Key : Unsigned_32 := 0;
   end record;

   package Cap_Vecs is new Ada.Containers.Vectors
     (Index_Type   => Natural,
      Element_Type => Cap_Pair);

   procedure Map_Decl_List_Ids
     (Ctx     : in out Context;
      Gen_DS  : TREE;
      Inst_DS : TREE);

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
            when DN_CONSTRAINED_ARRAY =>
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
            when DN_CONSTRAINED_RECORD =>
               Scan_Tree_For_Captures (Ctx, D (SM_NORMALIZED_DSCRMT_S, RT), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth + 1, Inst_Key, Inst_Gen_Id);
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
                  Scan_Tree_For_Captures (Ctx, Scan_B, SK, Owner_Map, Parent_Map, Captured_Vars, Nested_Subps, Target_Owner, Type_Depth, Inst_Key, Inst_Gen_Id);
                  return;
               end if;
            end;
         end;
      end if;

      if KIND (Node) in DN_PROCEDURE_CALL | DN_FUNCTION_CALL then
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
      if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY then
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
         end;
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
                     if not Is_Void_Tree (Def) and then KIND (Def) = DN_BLTN_OPERATOR_ID
                       and then DI (SM_OPERATOR, Def) = 19
                     then
                        return True;
                     end if;
                  end;
               end if;
            end if;
         end;
      elsif K = DN_AGGREGATE then
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
      if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY then
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

   procedure Emit_SS_Release_To (Ctx : in out Context; Target_Depth : Natural) is
      Dummy : Unbounded_String;
   begin
      if Natural (Ctx.SS_Marks.Length) > Target_Depth then
         Emit_Call_Or_Invoke
           (Ctx, "@__ada_ss_release",
            To_String (Ctx.SS_Marks.Element (Target_Depth)),
            "", Dummy, Nounwind => True);
      end if;
   end Emit_SS_Release_To;

   procedure Emit_SP_Restore_To (Ctx : in out Context; Target_Depth : Natural) is
   begin
      if Natural (Ctx.SP_Marks.Length) > Target_Depth then
         Emit_Inst (Ctx.E, "stackrestore " & To_String (Ctx.SP_Marks.Element (Target_Depth)));
      end if;
   end Emit_SP_Restore_To;

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
                                     or else Lib_Binder.Decl_List_Requires_Body (D (AS_DECL_S2, D (SM_SPEC, Gen_Id))))))
      then
         declare
            GSym_U : Unbounded_String;
         begin
            if Ctx.Gen_Elab_Flags.Contains (Gen_Key) then
               GSym_U := Ctx.Gen_Elab_Flags.Element (Gen_Key).Pir_Type;
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
         Saved_Exc     : constant Type_Maps.Map := Ctx.Gen_Local_Exc_Tags;
         Post_Spec_Exc : Type_Maps.Map := Ctx.Gen_Local_Exc_Tags;
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
                  Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (F1) & ", " & To_String (F2));
                  Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (L1) & ", " & To_String (L2));
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
                        Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (F1) & ", " & To_String (F2));
                        Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (L1) & ", " & To_String (L2));
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
               EEnv : constant Type_Maps.Map := Ctx.Gen_Env_Exc.Element (Gen_Key);
               Cur  : Type_Maps.Cursor := EEnv.First;
            begin
               while Type_Maps.Has_Element (Cur) loop
                  Ctx.Gen_Local_Exc_Tags.Include (Type_Maps.Key (Cur), Type_Maps.Element (Cur));
                  Type_Maps.Next (Cur);
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
                        if not Saved_Exc.Contains (Type_Maps.Key (C)) then
                           Post_Spec_Exc.Include (Type_Maps.Key (C), Type_Maps.Element (C));
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
                           PK : constant Unsigned_64 := U64_Nat_Maps.Key (C);
                           CI : constant Unsigned_32 := Unsigned_32 (Shift_Right (PK, 32) and 16#FFFF_FFFF#);
                           VK : constant Unsigned_32 := Unsigned_32 (PK and 16#FFFF_FFFF#);
                        begin
                           if CI = IK
                             and then not (Ctx.Gen_Env_Vars.Contains (Gen_Key) and then Ctx.Gen_Env_Vars.Element (Gen_Key).Contains (VK))
                           then
                              declare
                                 V : Var_Info;
                              begin
                                 if Ctx.Vars.Contains (VK) then
                                    V := Ctx.Vars.Element (VK);
                                 end if;
                                 V.Owner_Subp    := Ctx.Cur_Subp_Key;
                                 V.Captured_Slot := U64_Nat_Maps.Element (C);
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
                        Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                              Current_Exn   => Null_Unbounded_String,
                                              In_Handler    => False,
                                              SS_Mark       => Null_Unbounded_String,
                                              SP_Mark       => Null_Unbounded_String));
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
                     Scan_Label_Master_Depths (Ctx, D (AS_STM_S, Bdy_Block),
                       Natural (Ctx.Master_Stack.Length), Natural (Ctx.SS_Marks.Length), Natural (Ctx.SP_Marks.Length));
                     Scan_Label_Master_Depths (Ctx, Alt_S,
                       Natural (Ctx.Master_Stack.Length), Natural (Ctx.SS_Marks.Length), Natural (Ctx.SP_Marks.Length));
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

   -- Helper: populate a @__ada_arr_desc struct at Desc_Ptr
   procedure Emit_Store_Arr_Desc
     (Ctx      : in out Context;
      Desc_Ptr : String;
      Data_Ptr : String;
      Firsts   : Str_Array_4;
      Lasts    : Str_Array_4;
      ND       : Natural)
   is
      P0 : constant String := Next_Reg (Ctx.E, "adp0");
   begin
      Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & Desc_Ptr & ", 0");
      Emit_Inst (Ctx.E, "store.ptr " & Data_Ptr & ", " & P0 & ", align 8");
      for D_Idx in 1 .. Natural'Min (ND, 4) loop
         declare
            FP : constant String := Next_Reg (Ctx.E, "adfp");
            LP : constant String := Next_Reg (Ctx.E, "adlp");
         begin
            Emit_Inst (Ctx.E, FP & " = field_ptr @__ada_arr_desc, " & Desc_Ptr & ", " & Img (2 * D_Idx - 1));
            Emit_Inst (Ctx.E, "store.i32 " & To_String (Firsts (D_Idx)) & ", " & FP & ", align 4");
            Emit_Inst (Ctx.E, LP & " = field_ptr @__ada_arr_desc, " & Desc_Ptr & ", " & Img (2 * D_Idx));
            Emit_Inst (Ctx.E, "store.i32 " & To_String (Lasts (D_Idx)) & ", " & LP & ", align 4");
         end;
      end loop;
   end Emit_Store_Arr_Desc;

   -- =========================================================================
   -- Variable / Constant / Object Declarations
   -- =========================================================================

   procedure Emit_Object_Decl
     (Ctx        : in out Context;
      Decl       : TREE;
      Is_Global  : Boolean;
      Export_All : Boolean;
      Has_Dyn    : in out Boolean)
   is
      Is_Const : constant Boolean := KIND (Decl) = DN_CONSTANT_DECL;
      Names    : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Decl));
      Init_Exp : constant TREE := D (AS_EXP, Decl);
      VID      : TREE;
   begin
      while not IS_EMPTY (Names) loop
         POP (Names, VID);
         declare
            Can_Id   : constant TREE := Canonical_Def (VID);
            Key      : constant Unsigned_32 := Tree_Key (Can_Id);
            Ada_T    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, VID));
            TI       : constant Type_Info := Get_Type_Info (Ctx, Ada_T);
            Addr_Exp : TREE := D (SM_ADDRESS, VID);
            VInfo    : Var_Info;
         begin
            if Is_Void_Tree (Addr_Exp) and then Can_Id /= VID then
               Addr_Exp := D (SM_ADDRESS, Can_Id);
            end if;

            -- LRM 3.2.1 / 3.7.2 (c32001a/c/d/e, c32107a, c37210a, c37211e):
            -- Elaborate explicit subtype constraint BEFORE evaluating Init_Exp!
            if Ctx.Enable_Checks then
               declare
                  TD_N : constant TREE := D (AS_TYPE_DEF, Decl);
                  RT_O : constant TREE := Resolve_Type_Spec (Ada_T);
               begin
                  if not Is_Void_Tree (TD_N)
                    and then ((KIND (TD_N) = DN_SUBTYPE_INDICATION and then not Is_Void_Tree (D (AS_CONSTRAINT, TD_N)))
                              or else KIND (TD_N) = DN_CONSTRAINED_ARRAY_DEF)
                    and then not Is_Void_Tree (RT_O)
                    and then KIND (RT_O) in DN_CONSTRAINED_ACCESS | DN_CONSTRAINED_RECORD | DN_CONSTRAINED_ARRAY
                                          | DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED
                  then
                     Ctx.Saved_Type_Ranges.Exclude (Tree_Key (RT_O));
                     Ctx.Saved_Type_Discs.Exclude (Tree_Key (RT_O));
                     if KIND (RT_O) = DN_CONSTRAINED_ARRAY then
                        declare
                           RTI_O : constant Type_Info := Get_Type_Info (Ctx, RT_O);
                           ET_O  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, RTI_O.Elem_Type));
                        begin
                           for D_I in 1 .. RTI_O.NDims loop
                              if not Is_Void_Tree (RTI_O.Dims (D_I).Idx_Type) then
                                 Ctx.Saved_Type_Ranges.Exclude (Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, RTI_O.Dims (D_I).Idx_Type))));
                              end if;
                           end loop;
                           if not Is_Void_Tree (ET_O) and then KIND (ET_O) = DN_CONSTRAINED_ARRAY then
                              declare
                                 ETI_O : constant Type_Info := Get_Type_Info (Ctx, ET_O);
                              begin
                                 for D_I in 1 .. ETI_O.NDims loop
                                    if not Is_Void_Tree (ETI_O.Dims (D_I).Idx_Type) then
                                       Ctx.Saved_Type_Ranges.Exclude (Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, ETI_O.Dims (D_I).Idx_Type))));
                                    end if;
                                 end loop;
                              end;
                           end if;
                        end;
                     elsif KIND (RT_O) = DN_CONSTRAINED_ACCESS then
                        declare
                           DT_O : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, RT_O)));
                        begin
                           if not Is_Void_Tree (DT_O) then
                              Ctx.Saved_Type_Ranges.Exclude (Tree_Key (DT_O));
                              Ctx.Saved_Type_Discs.Exclude (Tree_Key (DT_O));
                              if KIND (DT_O) = DN_CONSTRAINED_ARRAY then
                                 declare
                                    DTI_O : constant Type_Info := Get_Type_Info (Ctx, DT_O);
                                 begin
                                    for D_I in 1 .. DTI_O.NDims loop
                                       if not Is_Void_Tree (DTI_O.Dims (D_I).Idx_Type) then
                                          Ctx.Saved_Type_Ranges.Exclude (Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, DTI_O.Dims (D_I).Idx_Type))));
                                       end if;
                                    end loop;
                                 end;
                              end if;
                           end if;
                        end;
                     end if;
                     declare
                        Mark_T : constant TREE :=
                          (if KIND (TD_N) = DN_SUBTYPE_INDICATION
                             and then not Is_Void_Tree (D (AS_NAME, TD_N))
                             and then not Is_Void_Tree (Get_Name_Defn (D (AS_NAME, TD_N)))
                             and then KIND (Get_Name_Defn (D (AS_NAME, TD_N))) in CLASS_TYPE_NAME
                           then D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, TD_N)))
                           else CONST_VOID);
                        Has_Real_C : constant Boolean :=
                          KIND (TD_N) = DN_SUBTYPE_INDICATION
                          and then not Is_Void_Tree (D (AS_CONSTRAINT, TD_N))
                          and then KIND (D (AS_CONSTRAINT, TD_N)) in DN_FLOAT_CONSTRAINT | DN_FIXED_CONSTRAINT;
                     begin
                        Emit_Type_Elab_Checks (Ctx, RT_O, Mark_Type => Mark_T, Has_Real_Constraint => Has_Real_C);
                        -- P3-3.13: Preserve per-object frozen range so A, B : Integer range 1..F don't overwrite A's bounds!
                        if Ctx.Saved_Type_Ranges.Contains (Tree_Key (RT_O)) then
                           Ctx.Saved_Var_Ranges.Include (Key, Ctx.Saved_Type_Ranges.Element (Tree_Key (RT_O)));
                        end if;
                     end;
                     -- LRM 3.6.1 & 4.1.4 (c35003a/b, c36104a/b, c36105b, c41401a, P3-3.13):
                     -- Check discrete subtype mark compatibility without re-evaluating dynamic index bounds!
                     if KIND (TD_N) in DN_CONSTRAINED_ARRAY_DEF | DN_SUBTYPE_INDICATION
                       and then not Is_Void_Tree (D (AS_CONSTRAINT, TD_N))
                       and then KIND (D (AS_CONSTRAINT, TD_N)) = DN_INDEX_CONSTRAINT
                       and then not Is_Void_Tree (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, TD_N)))
                     then
                        declare
                           DRL : SEQ_TYPE := LIST (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, TD_N)));
                           DRI : TREE;
                           RTI_IC : constant Type_Info := (if KIND (RT_O) = DN_CONSTRAINED_ARRAY then Get_Type_Info (Ctx, RT_O) else (others => <>));
                           Dim_IC : Natural := 0;
                           PT_U, Lo_U, Hi_U : Unbounded_String;
                           pragma Unreferenced (PT_U, Lo_U, Hi_U);
                        begin
                           while not IS_EMPTY (DRL) loop
                              POP (DRL, DRI);
                              Dim_IC := Dim_IC + 1;
                              if KIND (DRI) = DN_DISCRETE_SUBTYPE then
                                 declare
                                    Sub_I : constant TREE := D (AS_SUBTYPE_INDICATION, DRI);
                                    Con_I : constant TREE := (if not Is_Void_Tree (Sub_I) then D (AS_CONSTRAINT, Sub_I) else CONST_VOID);
                                    TM_I  : constant TREE := (if not Is_Void_Tree (Sub_I) then Get_Name_Defn (D (AS_NAME, Sub_I)) else CONST_VOID);
                                    Idx_T : constant TREE :=
                                      (if Dim_IC <= RTI_IC.NDims then Resolve_Type_Spec (Subst_Tree (Ctx, RTI_IC.Dims (Dim_IC).Idx_Type)) else CONST_VOID);
                                 begin
                                    if not Is_Void_Tree (Con_I) and then not Is_Void_Tree (TM_I) and then KIND (TM_I) in CLASS_TYPE_NAME
                                      and then not Is_Void_Tree (Idx_T) and then Ctx.Saved_Type_Ranges.Contains (Tree_Key (Idx_T))
                                    then
                                       -- Reuse already-saved bounds from Emit_Type_Elab_Checks to avoid double evaluation!
                                       declare
                                          SR  : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (Idx_T));
                                          MT  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, TM_I));
                                          MTI : constant Type_Info := Get_Type_Info (Ctx, MT);
                                          L0  : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                                          H0  : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                                          NE  : constant String := Next_Reg (Ctx.E, "dsne");
                                          L_C : constant String := Next_Label (Ctx.E, "dsc");
                                          L_O : constant String := Next_Label (Ctx.E, "dso");
                                       begin
                                          Emit_Inst (Ctx.E, NE & " = cmp_le_s." & To_String (SR.Pir_Type) & " " & L0 & ", " & H0);
                                          Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                                          Emit_Label (Ctx.E, L_C);
                                          Emit_Range_Check (Ctx, Emit_Int_Convert (Ctx, L0, To_String (SR.Pir_Type), To_String (MTI.Pir_Type)), To_String (MTI.Pir_Type), MT);
                                          Emit_Range_Check (Ctx, Emit_Int_Convert (Ctx, H0, To_String (SR.Pir_Type), To_String (MTI.Pir_Type)), To_String (MTI.Pir_Type), MT);
                                          Emit_Term (Ctx.E, "jmp " & L_O);
                                          Emit_Label (Ctx.E, L_O);
                                       end;
                                    else
                                       Emit_Discrete_Range (Ctx, DRI, PT_U, Lo_U, Hi_U);
                                    end if;
                                 end;
                              elsif KIND (DRI) = DN_RANGE_ATTRIBUTE then
                                 declare
                                    Idx_T : constant TREE :=
                                      (if Dim_IC <= RTI_IC.NDims then Resolve_Type_Spec (Subst_Tree (Ctx, RTI_IC.Dims (Dim_IC).Idx_Type)) else CONST_VOID);
                                 begin
                                    if Is_Void_Tree (Idx_T) or else not Ctx.Saved_Type_Ranges.Contains (Tree_Key (Idx_T)) then
                                       Emit_Discrete_Range (Ctx, DRI, PT_U, Lo_U, Hi_U);
                                    end if;
                                 end;
                              end if;
                           end loop;
                        end;
                     end if;
                  end if;
               end;
            end if;

            -- Explicit Ada 83 address clause: `for X use at Addr;`
            if not Is_Void_Tree (Addr_Exp) then
               declare
                  function Has_Unbound_Var (N : TREE; Depth : Natural := 0) return Boolean is
                  begin
                     if Is_Void_Tree (N) or else Depth > 24 then return False; end if;
                     case KIND (N) is
                        when DN_USED_OBJECT_ID =>
                           declare
                              D_N : constant TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, N)));
                           begin
                              if not Is_Void_Tree (D_N) and then KIND (D_N) in DN_VARIABLE_ID | DN_CONSTANT_ID
                                and then D (XD_REGION, D_N) = D (XD_REGION, Can_Id)
                                and then not Ctx.Vars.Contains (Tree_Key (D_N))
                              then
                                 return True;
                              end if;
                           end;
                           return False;
                        when DN_SELECTED =>
                           return Has_Unbound_Var (D (AS_NAME, N), Depth + 1)
                             or else Has_Unbound_Var (D (AS_DESIGNATOR, N), Depth + 1);
                        when DN_ATTRIBUTE =>
                           return Has_Unbound_Var (D (AS_NAME, N), Depth + 1);
                        when DN_PARENTHESIZED | DN_CONVERSION | DN_QUALIFIED | DN_ALL =>
                           return Has_Unbound_Var (D (AS_EXP, N), Depth + 1);
                        when DN_INDEXED =>
                           if Has_Unbound_Var (D (AS_NAME, N), Depth + 1) then return True; end if;
                           if not Is_Void_Tree (D (AS_EXP_S, N)) then
                              declare
                                 IL : SEQ_TYPE := LIST (D (AS_EXP_S, N));
                                 IE : TREE;
                              begin
                                 while not IS_EMPTY (IL) loop
                                    POP (IL, IE);
                                    if Has_Unbound_Var (IE, Depth + 1) then return True; end if;
                                 end loop;
                              end;
                           end if;
                           return False;
                        when DN_SLICE =>
                           return Has_Unbound_Var (D (AS_NAME, N), Depth + 1);
                        when DN_FUNCTION_CALL =>
                           declare
                              PS : constant TREE := D (SM_NORMALIZED_PARAM_S, N);
                              GS : constant TREE := (if not Is_Void_Tree (PS) then PS else D (AS_GENERAL_ASSOC_S, N));
                           begin
                              if not Is_Void_Tree (GS) then
                                 declare
                                    L : SEQ_TYPE := LIST (GS);
                                    E : TREE;
                                 begin
                                    while not IS_EMPTY (L) loop
                                       POP (L, E);
                                       if KIND (E) in DN_ASSOC | DN_NAMED then E := D (AS_EXP, E); end if;
                                       if Has_Unbound_Var (E, Depth + 1) then return True; end if;
                                    end loop;
                                 end;
                              end if;
                           end;
                           return False;
                        when others =>
                           return False;
                     end case;
                  end Has_Unbound_Var;

                  Defer_Addr : constant Boolean := Has_Unbound_Var (Addr_Exp);
                  PtrR : Unbounded_String;
               begin
                  if not Defer_Addr then
                     declare
                        AV  : constant Expr_Val := Emit_Expr (Ctx, Addr_Exp);
                        A64 : constant String := Emit_Int_Convert (Ctx, To_String (AV.Reg), To_String (AV.Pir_Type), "i64");
                        PR  : constant String := Next_Reg (Ctx.E, "acptr");
                     begin
                        Emit_Inst (Ctx.E, PR & " = inttoptr.ptr " & A64);
                        PtrR := To_Unbounded_String (PR);
                     end;
                  end if;
                  if Is_Global then
                     declare
                        Is_Gen_B : constant Boolean := Length (Ctx.Gen_Inst_Tag) > 0 and then not Is_Library_Level (Can_Id);
                        Do_Exp   : constant Boolean := Export_All and then not Is_Gen_B;
                        GSym     : constant String :=
                          (if Do_Exp then "@ada_aptr__" & Qualified_Ada_Name (Can_Id)
                           else "@ada_aptr__" & Qualified_Ada_Name (Can_Id) & "_" & Tree_Key_Str (Can_Id) & To_String (Ctx.Gen_Inst_Tag));
                        Link     : constant String := (if Do_Exp then "export " else "");
                        GA       : constant String := Next_Reg (Ctx.E, "gpa");
                     begin
                        Add_Global_Decl (Ctx.E, GSym,
                          Link & GSym & " = global ptr = zeroinit, align 8");
                        if not Defer_Addr then
                           Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                           Emit_Inst (Ctx.E, "store.ptr " & To_String (PtrR) & ", " & GA & ", align 8");
                        end if;
                        VInfo.Kind       := VK_Global_Ptr;
                        VInfo.Pir_Name   := To_Unbounded_String (GSym);
                        VInfo.Pir_Type   := TI.Pir_Type;
                        VInfo.Ada_Type   := Ada_T;
                        VInfo.Align      := TI.Align;
                        VInfo.Owner_Subp := 0;
                        VInfo.Has_Desc   := Defer_Addr;
                        Ctx.Vars.Include (Key, VInfo);
                     end;
                  elsif Defer_Addr then
                     declare
                        Slot : constant String := Next_Reg (Ctx.E, "aptr_" & Sanitize_Ident (Sym_Name (VID)) & "_");
                     begin
                        Emit_Entry_Alloca (Ctx.E, Slot & " = alloca ptr, align 8");
                        VInfo.Kind       := VK_Local_Ptr;
                        VInfo.Pir_Name   := To_Unbounded_String (Slot);
                        VInfo.Pir_Type   := TI.Pir_Type;
                        VInfo.Ada_Type   := Ada_T;
                        VInfo.Align      := TI.Align;
                        VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                        VInfo.Has_Desc   := True;
                        if Ctx.Vars.Contains (Key) and then Ctx.Vars.Element (Key).Owner_Subp = Ctx.Cur_Subp_Key then
                           VInfo.Captured_Slot := Ctx.Vars.Element (Key).Captured_Slot;
                        end if;
                        Ctx.Vars.Include (Key, VInfo);
                        if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                           declare
                              SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                              FP : constant String := Next_Reg (Ctx.E, "cfp");
                           begin
                              Emit_Inst (Ctx.E,
                                FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                              Emit_Inst (Ctx.E, "store.ptr " & Slot & ", " & FP & ", align 8");
                           end;
                        end if;
                     end;
                  else
                     VInfo.Kind       := VK_Local_Alloca;
                     VInfo.Pir_Name   := PtrR;
                     VInfo.Pir_Type   := TI.Pir_Type;
                     VInfo.Ada_Type   := Ada_T;
                     VInfo.Align      := TI.Align;
                     VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                     if Ctx.Vars.Contains (Key) and then Ctx.Vars.Element (Key).Owner_Subp = Ctx.Cur_Subp_Key then
                        VInfo.Captured_Slot := Ctx.Vars.Element (Key).Captured_Slot;
                     end if;
                     Ctx.Vars.Include (Key, VInfo);
                     if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                        declare
                           SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                           FP : constant String := Next_Reg (Ctx.E, "cfp");
                        begin
                           Emit_Inst (Ctx.E,
                             FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                             To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                           Emit_Inst (Ctx.E, "store.ptr " & To_String (PtrR) & ", " & FP & ", align 8");
                        end;
                     end if;
                  end if;
                  if not Defer_Addr then
                     if not Is_Void_Tree (Init_Exp) then
                        if TI.Kind = TK_Scalar then
                           declare
                              V  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                              VR : constant String :=
                                Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type));
                           begin
                              Emit_Range_Check (Ctx, VR, To_String (TI.Pir_Type), Ada_T);
                              Emit_Discriminant_Check (Ctx, VR, Ada_T, Is_Access => True);
                              Emit_Inst (Ctx.E,
                                "store." & To_String (TI.Pir_Type) & " " & VR &
                                ", " & To_String (PtrR) & ", align " & Img (TI.Align));
                           end;
                        else
                           Emit_Init_Composite (Ctx, To_String (PtrR), Ada_T, Init_Exp);
                           if TI.Kind = TK_Record then
                              Emit_Discriminant_Check (Ctx, To_String (PtrR), Ada_T, Is_Access => False);
                           end if;
                        end if;
                     elsif Is_Task_Type (Ada_T) then
                        Emit_Default_Init (Ctx, To_String (PtrR), Ada_T);
                     end if;
                  end if;
               end;

            -- Fast path for compile-time static scalar constants (check subtype range first! c32111b)
            elsif Is_Const and then TI.Kind = TK_Scalar and then not Is_Void_Tree (Init_Exp)
              and then Has_Static_Val (Safe_SM_Value (Init_Exp))
              and then not Is_Access_Type (Ada_T)
            then
               declare
                  Can_Fast : Boolean := True;
                  St       : Boolean := False;
                  Lo, Hi   : Integer_64 := 0;
               begin
                  if Is_Discrete_Type (Ada_T) then
                     Get_Discrete_Bounds (Ada_T, St, Lo, Hi);
                     if not St then
                        Can_Fast := False;
                     else
                        declare
                           Val_C : Integer_64;
                           Ovf_C : Boolean;
                        begin
                           Eval_Int64_Checked (Safe_SM_Value (Init_Exp), Val_C, Ovf_C);
                           if Ovf_C or else Val_C < Lo or else Val_C > Hi then
                              if Ctx.Enable_Checks then
                                 Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                                 declare
                                    Unr : constant String := Next_Label (Ctx.E, "sc_u");
                                 begin
                                    Emit_Label (Ctx.E, Unr);
                                 end;
                              end if;
                           end if;
                        end;
                     end if;
                  else
                     Can_Fast := False;
                  end if;
                  if not Can_Fast and then Ctx.Enable_Checks then
                     declare
                        V  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                        VR : constant String := Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type));
                     begin
                        Emit_Range_Check (Ctx, VR, To_String (TI.Pir_Type), Ada_T);
                     end;
                  end if;
               end;
               VInfo.Ada_Type := Ada_T;
               VInfo.Pir_Type := TI.Pir_Type;
               VInfo.Align    := TI.Align;
               VInfo.Owner_Subp := 0;
               if Is_Float_Type (Ada_T) then
                  VInfo.Kind      := VK_Const_Float;
                  VInfo.Const_F64 := Eval_Float64 (Safe_SM_Value (Init_Exp));
               elsif Is_Fixed_Type (Ada_T) then
                  VInfo.Kind := VK_Const_Int;
                  declare
                     Tick_V : Integer_64;
                     Tick_O : Boolean;
                  begin
                     Eval_Fixed_Ticks_Exact_Checked (Safe_SM_Value (Init_Exp), Ada_T, Tick_V, Tick_O);
                     if Tick_O and then Ctx.Enable_Checks then
                        Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                        declare
                           Unr : constant String := Next_Label (Ctx.E, "scx_u");
                        begin
                           Emit_Label (Ctx.E, Unr);
                        end;
                     end if;
                     VInfo.Const_I64 := Tick_V;
                  end;
               else
                  VInfo.Kind      := VK_Const_Int;
                  VInfo.Const_I64 := Eval_Int64 (Safe_SM_Value (Init_Exp));
               end if;
               Ctx.Vars.Include (Key, VInfo);
               -- LRM 7.4.3: for exported library-level scalar constants (including full deferred constants & lib generic inst specs),
               -- also define and initialize the exported global so separately-compiled clients don't load zeroinit!
               if Is_Global and then Export_All and then Is_Library_Level (Can_Id) then
                  declare
                     GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_Id);
                     GA   : constant String := Next_Reg (Ctx.E, "scga");
                     ValS : constant String :=
                       (if VInfo.Kind = VK_Const_Float then Format_Float64 (VInfo.Const_F64)
                        else Img (Long_Long_Integer (VInfo.Const_I64)));
                     CV   : constant String := Next_Reg (Ctx.E, "scv");
                  begin
                     Add_Global_Decl (Ctx.E, GSym,
                       "export " & GSym & " = global " & To_String (TI.Pir_Type) & " = zeroinit, align " & Img (TI.Align));
                     Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                     Emit_Inst (Ctx.E, CV & " = const." & To_String (TI.Pir_Type) & " " & ValS);
                     Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & CV & ", " & GA & ", align " & Img (TI.Align));
                  end;
               end if;

            elsif Is_Global and then (TI.Kind = TK_Unconstrained_Array or else (TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic)) then
               declare
                  Is_Gen_B : constant Boolean := Length (Ctx.Gen_Inst_Tag) > 0 and then not Is_Library_Level (Can_Id);
                  Do_Exp   : constant Boolean := Export_All and then not Is_Gen_B;
                  GSym     : constant String :=
                    (if Do_Exp then "@ada_g__" & Qualified_Ada_Name (Can_Id)
                     else "@ada_g__" & Qualified_Ada_Name (Can_Id) & "_" & Tree_Key_Str (Can_Id) & To_String (Ctx.Gen_Inst_Tag));
                  Link     : constant String := (if Do_Exp then "export " else "");
                  GA   : constant String := Next_Reg (Ctx.E, "ga");
                  ETI  : constant Type_Info := Get_Type_Info (Ctx, TI.Elem_Type);
                  EAl  : constant Natural := Elem_Storage_Align (ETI);
                  ESz  : constant Natural := Elem_Storage_Size (ETI);
                  ND   : Natural := Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims));
                  Data_Ptr : Unbounded_String;
               begin
                  Add_Global_Decl (Ctx.E, GSym,
                    Link & GSym & " = global @__ada_arr_desc = zeroinit, align 8");
                  VInfo.Kind        := VK_Dyn_Array;
                  VInfo.Pir_Name    := To_Unbounded_String (GSym);
                  VInfo.Pir_Type    := To_Unbounded_String ("ptr");
                  VInfo.Ada_Type    := Ada_T;
                  VInfo.Align       := EAl;
                  VInfo.Owner_Subp  := 0;
                  VInfo.Has_Desc    := True;
                  VInfo.Desc_Global := To_Unbounded_String (GSym);

                  if TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic
                    and then not (Is_Const and then Is_Void_Tree (Init_Exp))
                  then
                     declare
                        Sub_S : constant TREE := D (SM_INDEX_SUBTYPE_S, Resolve_Type_Spec (Ada_T));
                        IL    : SEQ_TYPE := LIST (Sub_S);
                        TD_N  : constant TREE := D (AS_TYPE_DEF, Decl);
                        DR_L  : SEQ_TYPE :=
                          (if not Is_Void_Tree (TD_N) and then KIND (TD_N) = DN_SUBTYPE_INDICATION
                             and then not Is_Void_Tree (D (AS_CONSTRAINT, TD_N))
                             and then KIND (D (AS_CONSTRAINT, TD_N)) = DN_INDEX_CONSTRAINT
                           then LIST (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, TD_N)))
                           elsif not Is_Void_Tree (TD_N) and then KIND (TD_N) = DN_CONSTRAINED_ARRAY_DEF
                             and then not Is_Void_Tree (D (AS_CONSTRAINT, TD_N))
                             and then KIND (D (AS_CONSTRAINT, TD_N)) = DN_INDEX_CONSTRAINT
                           then LIST (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, TD_N)))
                           else MAKE);
                        Idx_T, DR_N : TREE;
                        Dim   : Natural := 0;
                        Tot64 : Unbounded_String;
                        Byt   : Unbounded_String;
                        Nz    : constant String := Next_Reg (Ctx.E, "gnz");
                        AlB   : constant String := Next_Reg (Ctx.E, "gal");
                     begin
                        while not IS_EMPTY (IL) and then Dim < Max_Array_Dims loop
                           POP (IL, Idx_T);
                           if not IS_EMPTY (DR_L) then
                              POP (DR_L, DR_N);
                           else
                              DR_N := CONST_VOID;
                           end if;
                           Dim := Dim + 1;
                           declare
                              RT, RL, RH : Unbounded_String;
                              F32, L32   : Unbounded_String;
                              Uns_I      : Boolean;
                           begin
                              if not Is_Void_Tree (DR_N) and then KIND (DR_N) = DN_RANGE_ATTRIBUTE then
                                 Emit_Discrete_Range (Ctx, DR_N, RT, RL, RH);
                                 Uns_I := Is_Unsigned_Scalar (Idx_T, To_String (RT));
                                 F32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32", Is_Unsigned => Uns_I));
                                 L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32", Is_Unsigned => Uns_I));
                              else
                                 Emit_Index_Bounds (Ctx, Idx_T, F32, L32);
                              end if;
                              VInfo.First_Regs (Dim) := F32;
                              VInfo.Last_Regs (Dim)  := L32;
                           end;
                        end loop;
                        ND := Dim;
                        VInfo.NDims := ND;
                        Emit_Array_Byte_Size_I64 (Ctx, VInfo.First_Regs, VInfo.Last_Regs, ND, ESz, Tot64, Byt);
                        Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                        Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", Data_Ptr);
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        Emit_Store_Arr_Desc (Ctx, GA, To_String (Data_Ptr), VInfo.First_Regs, VInfo.Last_Regs, ND);
                        Ctx.Vars.Include (Key, VInfo);
                        if not Is_Void_Tree (Init_Exp) then
                           Emit_Init_Composite (Ctx, To_String (Data_Ptr), Ada_T, Init_Exp, VInfo.First_Regs, VInfo.Last_Regs);
                        elsif Is_Access_Type (Subst_Resolve_Type (Ctx, TI.Elem_Type)) or else ETI.Kind = TK_Record
                          or else Is_Task_Type (Subst_Resolve_Type (Ctx, TI.Elem_Type))
                          or else ETI.Kind = TK_Constrained_Array
                        then
                           declare
                              L_H : constant String := Next_Label (Ctx.E, "gdi_h");
                              L_B : constant String := Next_Label (Ctx.E, "gdi_b");
                              L_E : constant String := Next_Label (Ctx.E, "gdi_e");
                              IR  : constant String := Next_Reg (Ctx.E, "gdi");
                              Cnd : constant String := Next_Reg (Ctx.E, "gdc");
                              EP  : constant String := Next_Reg (Ctx.E, "gdep");
                              Nx  : constant String := Next_Reg (Ctx.E, "gdnx");
                           begin
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              declare
                                 EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (Data_Ptr), IR);
                              begin
                                 Emit_Default_Init (Ctx, EP2, TI.Elem_Type, Activate_Tasks => False);
                              end;
                              pragma Unreferenced (EP);
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                           end;
                        end if;
                     end;
                  elsif not Is_Void_Tree (Init_Exp) then
                     declare
                        Src   : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                        Tot64 : Unbounded_String;
                        Byt   : Unbounded_String;
                        Nz    : constant String := Next_Reg (Ctx.E, "gnz");
                        AlB   : constant String := Next_Reg (Ctx.E, "gal");
                     begin
                        ND := Natural'Max (1, Natural'Min ((if Src.NDims > 0 then Src.NDims else ND), Max_Array_Dims));
                        VInfo.NDims := ND;
                        for D_Idx in 1 .. ND loop
                           Get_Array_Dim_Bounds (Ctx, Src, D_Idx, VInfo.First_Regs (D_Idx), VInfo.Last_Regs (D_Idx));
                        end loop;
                        Emit_Array_Byte_Size_I64 (Ctx, VInfo.First_Regs, VInfo.Last_Regs, ND, ESz, Tot64, Byt);
                        Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                        Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", Data_Ptr);
                        Emit_Copy_Composite (Ctx, To_String (Data_Ptr), Src, Ada_T);
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        Emit_Store_Arr_Desc (Ctx, GA, To_String (Data_Ptr), VInfo.First_Regs, VInfo.Last_Regs, ND);
                        Ctx.Vars.Include (Key, VInfo);
                     end;
                  else
                     VInfo.NDims := ND;
                     Ctx.Vars.Include (Key, VInfo);
                  end if;
               end;

            elsif Is_Global then
               declare
                  Is_Gen_B : constant Boolean := Length (Ctx.Gen_Inst_Tag) > 0 and then not Is_Library_Level (Can_Id);
                  Do_Exp   : constant Boolean := Export_All and then not Is_Gen_B;
                  GSym     : constant String :=
                    (if Do_Exp then "@ada_g__" & Qualified_Ada_Name (Can_Id)
                     else "@ada_g__" & Qualified_Ada_Name (Can_Id) & "_" & Tree_Key_Str (Can_Id) & To_String (Ctx.Gen_Inst_Tag));
                  Link     : constant String := (if Do_Exp then "export " else "");
               begin
                  Add_Global_Decl (Ctx.E, GSym,
                    Link & GSym & " = global " & To_String (TI.Pir_Type) & " = zeroinit, align " & Img (TI.Align));
                  VInfo.Kind       := VK_Global;
                  VInfo.Pir_Name   := To_Unbounded_String (GSym);
                  VInfo.Pir_Type   := TI.Pir_Type;
                  VInfo.Ada_Type   := Ada_T;
                  VInfo.Align      := TI.Align;
                  VInfo.Owner_Subp := 0;
                  Ctx.Vars.Include (Key, VInfo);

                  if not Is_Void_Tree (Init_Exp) then
                     declare
                        GA : constant String := Next_Reg (Ctx.E, "ga");
                     begin
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        if TI.Kind = TK_Scalar then
                           declare
                              V  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                              VR : constant String :=
                                Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type));
                           begin
                              Emit_Range_Check (Ctx, VR, To_String (TI.Pir_Type), Ada_T);
                              Emit_Discriminant_Check (Ctx, VR, Ada_T, Is_Access => True);
                              Emit_Inst (Ctx.E,
                                "store." & To_String (TI.Pir_Type) & " " & VR &
                                ", " & GA & ", align " & Img (TI.Align));
                           end;
                        elsif TI.Kind = TK_Constrained_Array and then KIND (Init_Exp) /= DN_AGGREGATE then
                           declare
                              SV  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                              Tgt : Expr_Val;
                              ND_A : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims));
                           begin
                              Tgt.Kind := EK_Slice; Tgt.Reg := To_Unbounded_String (GA); Tgt.Ada_Type := Ada_T; Tgt.NDims := ND_A;
                              for D_I in 1 .. ND_A loop
                                 Emit_Index_Bounds (Ctx, TI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                              end loop;
                              Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                              Emit_Copy_Composite (Ctx, GA, SV, Ada_T);
                           end;
                        else
                           Emit_Init_Composite (Ctx, GA, Ada_T, Init_Exp);
                           if TI.Kind = TK_Record then
                              Emit_Discriminant_Check (Ctx, GA, Ada_T, Is_Access => False);
                           end if;
                        end if;
                     end;
                  elsif not (Is_Const and then Is_Void_Tree (Init_Exp))
                    and then (TI.Kind = TK_Record or else Is_Task_Type (Subst_Resolve_Type (Ctx, Ada_T)) or else (TI.Kind = TK_Constrained_Array and then not TI.Is_Dynamic))
                  then
                     declare
                        GA : constant String := Next_Reg (Ctx.E, "ga");
                     begin
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        Emit_Default_Init (Ctx, GA, Ada_T);
                     end;
                  end if;
               end;

            elsif TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic then
               -- Dynamic-bound local array -> PIR alloca_dyn!
               Has_Dyn := True;
               declare
                  ETI    : constant Type_Info := Get_Type_Info (Ctx, TI.Elem_Type);
                  EAl    : constant Natural := Elem_Storage_Align (ETI);
                  ESz    : constant Natural := Elem_Storage_Size (ETI);
                  Sub_S  : constant TREE := D (SM_INDEX_SUBTYPE_S, Resolve_Type_Spec (Ada_T));
                  IL     : SEQ_TYPE := LIST (Sub_S);
                  Idx_T  : TREE;
                  Dim    : Natural := 0;
                  Tot64  : Unbounded_String;
                  Byt    : Unbounded_String;
               begin
                  VInfo.Kind       := VK_Dyn_Array;
                  VInfo.Ada_Type   := Ada_T;
                  VInfo.Pir_Type   := To_Unbounded_String ("ptr");
                  VInfo.Align      := EAl;
                  VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                  declare
                     TD_N : constant TREE := D (AS_TYPE_DEF, Decl);
                     DR_L : SEQ_TYPE :=
                       (if not Is_Void_Tree (TD_N) and then KIND (TD_N) = DN_SUBTYPE_INDICATION
                          and then not Is_Void_Tree (D (AS_CONSTRAINT, TD_N))
                          and then KIND (D (AS_CONSTRAINT, TD_N)) = DN_INDEX_CONSTRAINT
                        then LIST (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, TD_N)))
                        elsif not Is_Void_Tree (TD_N) and then KIND (TD_N) = DN_CONSTRAINED_ARRAY_DEF
                          and then not Is_Void_Tree (D (AS_CONSTRAINT, TD_N))
                          and then KIND (D (AS_CONSTRAINT, TD_N)) = DN_INDEX_CONSTRAINT
                        then LIST (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, TD_N)))
                        else MAKE);
                     DR_N : TREE;
                  begin
                  while not IS_EMPTY (IL) and then Dim < Max_Array_Dims loop
                     POP (IL, Idx_T);
                     if not IS_EMPTY (DR_L) then
                        POP (DR_L, DR_N);
                     else
                        DR_N := CONST_VOID;
                     end if;
                     Dim := Dim + 1;
                     declare
                        RT, RL, RH : Unbounded_String;
                        F32, L32   : Unbounded_String;
                        Uns_I      : Boolean;
                     begin
                        if not Is_Void_Tree (DR_N) and then KIND (DR_N) = DN_RANGE_ATTRIBUTE then
                           Emit_Discrete_Range (Ctx, DR_N, RT, RL, RH);
                           Uns_I := Is_Unsigned_Scalar (Idx_T, To_String (RT));
                           F32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32", Is_Unsigned => Uns_I));
                           L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32", Is_Unsigned => Uns_I));
                        else
                           Emit_Index_Bounds (Ctx, Idx_T, F32, L32);
                        end if;
                        VInfo.First_Regs (Dim) := F32;
                        VInfo.Last_Regs (Dim)  := L32;
                     end;
                  end loop;
                  end;
                  VInfo.NDims := Dim;
                  Emit_Array_Byte_Size_I64 (Ctx, VInfo.First_Regs, VInfo.Last_Regs, Dim, ESz, Tot64, Byt);
                  declare
                     Too : constant String := Next_Reg (Ctx.E, "dbyt");
                     Nz  : constant String := Next_Reg (Ctx.E, "dnz");
                     AlB : constant String := Next_Reg (Ctx.E, "dal");
                     Ptr : constant String := Next_Reg (Ctx.E, "darr");
                  begin
                     if Ctx.Enable_Checks then
                        -- Raise STORAGE_ERROR on huge dynamic stack array (c52103x)
                        declare
                           L_F : constant String := Next_Label (Ctx.E, "dbf");
                           L_K : constant String := Next_Label (Ctx.E, "dbk");
                        begin
                           Emit_Inst (Ctx.E, Too & " = cmp_gt_s.i64 " & To_String (Byt) & ", 4194304");
                           Emit_Term (Ctx.E, "br " & Too & ", " & L_F & ", " & L_K);
                           Emit_Label (Ctx.E, L_F);
                           Emit_Raise_Tag (Ctx, Storage_Error_Tag (Ctx));
                           Emit_Label (Ctx.E, L_K);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                     Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                     Emit_Inst (Ctx.E, Ptr & " = alloca_dyn " & AlB & ", align " & Img (EAl));
                     VInfo.Pir_Name := To_Unbounded_String (Ptr);
                     if Ctx.Vars.Contains (Key) and then Ctx.Vars.Element (Key).Owner_Subp = Ctx.Cur_Subp_Key then
                        VInfo.Captured_Slot := Ctx.Vars.Element (Key).Captured_Slot;
                     end if;
                     if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                        declare
                           SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                           DR : constant String := Next_Reg (Ctx.E, "cdesc");
                           FP : constant String := Next_Reg (Ctx.E, "cfp");
                        begin
                           Emit_Entry_Alloca (Ctx.E, DR & " = alloca @__ada_arr_desc, align 8");
                           Emit_Store_Arr_Desc (Ctx, DR, Ptr, VInfo.First_Regs, VInfo.Last_Regs, Dim);
                           Emit_Inst (Ctx.E,
                             FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                             To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                           Emit_Inst (Ctx.E, "store.ptr " & DR & ", " & FP & ", align 8");
                           VInfo.Has_Desc := True;
                        end;
                     end if;
                     Ctx.Vars.Include (Key, VInfo);
                     if not Is_Void_Tree (Init_Exp) then
                        -- P3-3.7: Named array aggregate without `others` slides in object declaration!
                        declare
                           function Obj_Agg_Named_No_Others (AN : TREE; Rem_D : Natural) return Boolean is
                           begin
                              if Rem_D = 0 then return True; end if;
                              if Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then return False; end if;
                              declare
                                 AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                                 IT : TREE;
                                 Has_Any : Boolean := False;
                              begin
                                 while not IS_EMPTY (AL) loop
                                    POP (AL, IT);
                                    Has_Any := True;
                                    if KIND (IT) not in DN_ASSOC | DN_NAMED then return False; end if;
                                    declare
                                       CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                       Ch : TREE;
                                    begin
                                       while not IS_EMPTY (CS) loop
                                          POP (CS, Ch);
                                          if KIND (Ch) = DN_CHOICE_OTHERS then return False; end if;
                                       end loop;
                                    end;
                                    if Rem_D > 1 and then not Obj_Agg_Named_No_Others (D (AS_EXP, IT), Rem_D - 1) then
                                       return False;
                                    end if;
                                 end loop;
                                 return Has_Any;
                              end;
                           end Obj_Agg_Named_No_Others;
                        begin
                           if KIND (Init_Exp) = DN_AGGREGATE and then Obj_Agg_Named_No_Others (Init_Exp, Dim) then
                              declare
                                 SV  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Subst_Base_Type (Ctx, Ada_T));
                                 Tgt : Expr_Val;
                              begin
                                 Tgt.Kind := EK_Slice; Tgt.Reg := To_Unbounded_String (Ptr); Tgt.Ada_Type := Ada_T;
                                 Tgt.NDims := Dim; Tgt.First_Regs := VInfo.First_Regs; Tgt.Last_Regs := VInfo.Last_Regs;
                                 Emit_Array_Length_Check (Ctx, Tgt, SV, Dim);
                                 Emit_Copy_Composite (Ctx, Ptr, SV, Ada_T);
                              end;
                           else
                              Emit_Init_Composite (Ctx, Ptr, Ada_T, Init_Exp, VInfo.First_Regs, VInfo.Last_Regs);
                           end if;
                        end;
                     elsif Is_Access_Type (Subst_Resolve_Type (Ctx, TI.Elem_Type)) or else ETI.Kind = TK_Record
                       or else Is_Task_Type (Subst_Resolve_Type (Ctx, TI.Elem_Type))
                       or else ETI.Kind = TK_Constrained_Array
                     then
                        declare
                           L_H : constant String := Next_Label (Ctx.E, "ddi_h");
                           L_B : constant String := Next_Label (Ctx.E, "ddi_b");
                           L_E : constant String := Next_Label (Ctx.E, "ddi_e");
                           IR  : constant String := Next_Reg (Ctx.E, "ddi");
                           Cnd : constant String := Next_Reg (Ctx.E, "ddc");
                           EP  : constant String := Next_Reg (Ctx.E, "ddep");
                           Nx  : constant String := Next_Reg (Ctx.E, "ddnx");
                        begin
                           Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                           Emit_Label (Ctx.E, L_H, IR & ": i64");
                           Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                           Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                           Emit_Label (Ctx.E, L_B);
                           declare
                              EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, Ptr, IR);
                           begin
                              Emit_Default_Init (Ctx, EP2, TI.Elem_Type, Activate_Tasks => False);
                           end;
                           pragma Unreferenced (EP);
                           Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                           Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                           Emit_Label (Ctx.E, L_E);
                        end;
                     end if;
                  end;
               end;

            elsif TI.Kind = TK_Unconstrained_Array and then not Is_Void_Tree (Init_Exp) then
               -- Unconstrained constant/variable initialized from an expression (e.g. S : String := Make(N);)
               declare
                  Src : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                  ND  : constant Natural := (if Src.NDims > 0 then Src.NDims else (if TI.NDims > 0 then TI.NDims else 1));
                  Data_Ptr : Unbounded_String := Src.Reg;
               begin
                  VInfo.Kind       := VK_Dyn_Array;
                  VInfo.Pir_Type   := To_Unbounded_String ("ptr");
                  VInfo.Ada_Type   := Ada_T;
                  VInfo.Align      := Src.Align;
                  VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                  VInfo.NDims      := ND;
                  for D_Idx in 1 .. ND loop
                     Get_Array_Dim_Bounds (Ctx, Src, D_Idx, VInfo.First_Regs (D_Idx), VInfo.Last_Regs (D_Idx));
                  end loop;

                  -- Always copy non-literal unconstrained initializers (even for `constant String := S;`)
                  -- so mutating S later never mutates the constant C! (LRM 3.2.1)
                  if not Is_Const or else KIND (Init_Exp) /= DN_STRING_LITERAL then
                     declare
                        ETI   : constant Type_Info := Get_Type_Info (Ctx, TI.Elem_Type);
                        EAl   : constant Natural := Elem_Storage_Align (ETI);
                        ESz   : constant Natural := Elem_Storage_Size (ETI);
                        Tot64 : Unbounded_String;
                        Byt   : Unbounded_String;
                        Buf_U : Unbounded_String;
                     begin
                        Emit_Array_Byte_Size_I64 (Ctx, VInfo.First_Regs, VInfo.Last_Regs, ND, ESz, Tot64, Byt);
                        Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", To_String (Byt) & ", " & Img (EAl), "ptr", Buf_U);
                        Emit_Copy_Composite (Ctx, To_String (Buf_U), Src, Ada_T);
                        Data_Ptr := Buf_U;
                        VInfo.Align := EAl;
                     end;
                  end if;

                  VInfo.Pir_Name := Data_Ptr;
                  if Ctx.Vars.Contains (Key) and then Ctx.Vars.Element (Key).Owner_Subp = Ctx.Cur_Subp_Key then
                     VInfo.Captured_Slot := Ctx.Vars.Element (Key).Captured_Slot;
                  end if;
                  if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                     declare
                        SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                        DR : constant String := Next_Reg (Ctx.E, "cdesc");
                        FP : constant String := Next_Reg (Ctx.E, "cfp");
                     begin
                        Emit_Entry_Alloca (Ctx.E, DR & " = alloca @__ada_arr_desc, align 8");
                        Emit_Store_Arr_Desc (Ctx, DR, To_String (Data_Ptr), VInfo.First_Regs, VInfo.Last_Regs, ND);
                        Emit_Inst (Ctx.E,
                          FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                          To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                        Emit_Inst (Ctx.E, "store.ptr " & DR & ", " & FP & ", align 8");
                        VInfo.Has_Desc := True;
                     end;
                  end if;
                  Ctx.Vars.Include (Key, VInfo);
               end;

            else
               -- Standard local fixed alloca (hoisted to .entry for mem2reg!)
               declare
                  Reg : constant String := Next_Reg (Ctx.E, "v_" & Sanitize_Ident (Sym_Name (VID)) & "_");
               begin
                  Emit_Entry_Alloca (Ctx.E, Reg & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                  VInfo.Kind       := VK_Local_Alloca;
                  VInfo.Pir_Name   := To_Unbounded_String (Reg);
                  VInfo.Pir_Type   := TI.Pir_Type;
                  VInfo.Ada_Type   := Ada_T;
                  VInfo.Align      := TI.Align;
                  VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                  if Ctx.Vars.Contains (Key) and then Ctx.Vars.Element (Key).Owner_Subp = Ctx.Cur_Subp_Key then
                     VInfo.Captured_Slot := Ctx.Vars.Element (Key).Captured_Slot;
                  end if;
                  Ctx.Vars.Include (Key, VInfo);

                  -- If this variable is captured by a nested subprogram, store its address in our frame!
                  if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                     declare
                        SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                        FP : constant String := Next_Reg (Ctx.E, "cfp");
                     begin
                        Emit_Inst (Ctx.E,
                          FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                          To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                        Emit_Inst (Ctx.E, "store.ptr " & Reg & ", " & FP & ", align 8");
                     end;
                  end if;

                  if not Is_Void_Tree (Init_Exp) then
                     if TI.Kind = TK_Scalar then
                        declare
                           V  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                           VR : constant String :=
                             Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type));
                        begin
                           Emit_Range_Check (Ctx, VR, To_String (TI.Pir_Type), Ada_T);
                           Emit_Discriminant_Check (Ctx, VR, Ada_T, Is_Access => True);
                           Emit_Inst (Ctx.E,
                             "store." & To_String (TI.Pir_Type) & " " & VR &
                             ", " & Reg & ", align " & Img (TI.Align));
                        end;
                     elsif TI.Kind = TK_Constrained_Array then
                        declare
                           function Obj_Agg_Named_No_Others2 (AN : TREE; Rem_D : Natural) return Boolean is
                           begin
                              if Rem_D = 0 then return True; end if;
                              if Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then return False; end if;
                              declare
                                 AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                                 IT : TREE;
                                 Has_Any : Boolean := False;
                              begin
                                 while not IS_EMPTY (AL) loop
                                    POP (AL, IT);
                                    Has_Any := True;
                                    if KIND (IT) not in DN_ASSOC | DN_NAMED then return False; end if;
                                    declare
                                       CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                       Ch : TREE;
                                    begin
                                       while not IS_EMPTY (CS) loop
                                          POP (CS, Ch);
                                          if KIND (Ch) = DN_CHOICE_OTHERS then return False; end if;
                                       end loop;
                                    end;
                                    if Rem_D > 1 and then not Obj_Agg_Named_No_Others2 (D (AS_EXP, IT), Rem_D - 1) then
                                       return False;
                                    end if;
                                 end loop;
                                 return Has_Any;
                              end;
                           end Obj_Agg_Named_No_Others2;
                           ND_A : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims));
                        begin
                           if KIND (Init_Exp) /= DN_AGGREGATE or else Obj_Agg_Named_No_Others2 (Init_Exp, ND_A) then
                              declare
                                 Eval_T : constant TREE :=
                                   (if KIND (Init_Exp) = DN_AGGREGATE then Subst_Base_Type (Ctx, Ada_T) else Ada_T);
                                 SV  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Eval_T);
                                 Tgt : Expr_Val;
                              begin
                                 Tgt.Kind := EK_Slice; Tgt.Reg := To_Unbounded_String (Reg); Tgt.Ada_Type := Ada_T; Tgt.NDims := ND_A;
                                 for D_I in 1 .. ND_A loop
                                    Emit_Index_Bounds (Ctx, TI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                                 end loop;
                                 Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                                 Emit_Copy_Composite (Ctx, Reg, SV, Ada_T);
                              end;
                           else
                              Emit_Init_Composite (Ctx, Reg, Ada_T, Init_Exp);
                           end if;
                        end;
                     else
                        Emit_Init_Composite (Ctx, Reg, Ada_T, Init_Exp);
                        if TI.Kind = TK_Record then
                           Emit_Discriminant_Check (Ctx, Reg, Ada_T, Is_Access => False);
                        end if;
                     end if;
                  elsif Is_Access_Type (Subst_Resolve_Type (Ctx, Ada_T)) then
                     -- Ada guarantees default null initialization for access objects!
                     Emit_Inst (Ctx.E, "store.ptr 0, " & Reg & ", align 8");
                  elsif not (Is_Const and then Is_Void_Tree (Init_Exp))
                    and then (TI.Kind = TK_Record or else Is_Task_Type (Subst_Resolve_Type (Ctx, Ada_T)) or else (TI.Kind = TK_Constrained_Array and then not TI.Is_Dynamic))
                  then
                     Emit_Default_Init (Ctx, Reg, Ada_T);
                  end if;
               end;
            end if;
         end;
      end loop;
   end Emit_Object_Decl;

   -- =========================================================================
   -- Declarative Part Processing (2-pass: specs/generics then bodies/objects)
   -- =========================================================================

   procedure Pre_Register_Subp
     (Ctx         : in out Context;
      Subp_Id     : TREE;
      Spec_Node   : TREE;
      Parent_Subp : Unsigned_32;
      Is_Subunit  : Boolean := False)
   is
      Can : constant TREE := Canonical_Def (Subp_Id);
      Key : constant Unsigned_32 := Tree_Key (Can);
      SI  : Subp_Info;
   begin
      if Ctx.Subps.Contains (Key) then
         SI := Ctx.Subps.Element (Key);
      end if;
      if Length (SI.Pir_Name) = 0
        or else (Length (Ctx.Gen_Inst_Tag) > 0
                 and then Ada.Strings.Fixed.Index (To_String (SI.Pir_Name), To_String (Ctx.Gen_Inst_Tag)) = 0
                 and then To_String (Ctx.Gen_Inst_Tag) /= "_gi" & Tree_Key_Str (Can)
                 and then not Is_Subunit and then not Is_Library_Level (Can))
      then
         -- Reset stale metadata (Needs_SL, Has_Frame, Frame_Type, Elab_Flag) when re-registering under a new generic instantiation!
         SI := (others => <>);
         SI.Pir_Name := To_Unbounded_String (Mangle_Subp_Name (Ctx, Can, Spec_Node, Is_Subunit => Is_Subunit));
      end if;
      SI.Spec_Node      := Spec_Node;
      SI.Parent_Subp    := Parent_Subp;
      SI.Is_C_Interface := Is_C_Interfaced (Can);
      if SI.Is_C_Interface then
         SI.Needs_SL := False;
      elsif Parent_Subp /= 0 and then Ctx.Subps.Contains (Parent_Subp)
        and then (Ctx.Subps.Element (Parent_Subp).Has_Frame or else Ctx.Subps.Element (Parent_Subp).Needs_SL)
      then
         SI.Needs_SL := True;
      end if;
      SI.Is_Function := KIND (Spec_Node) = DN_FUNCTION_SPEC;
      if SI.Is_Function then
         declare
            RDef : constant TREE := Get_Name_Defn (D (AS_NAME, Spec_Node));
         begin
            SI.Ret_Type := (if not Is_Void_Tree (RDef) then Subst_Tree (Ctx, D (SM_TYPE_SPEC, RDef)) else CONST_VOID);
         end;
         declare
            RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
         begin
            SI.Ret_Composite := RTI.Kind /= TK_Scalar;
            SI.Ret_Uncon_Arr := RTI.Kind = TK_Unconstrained_Array
              or else (RTI.Kind = TK_Constrained_Array and then RTI.Is_Dynamic);
         end;
      end if;
      Ctx.Subps.Include (Key, SI);
   end Pre_Register_Subp;

   procedure Emit_Decl_Items
     (Ctx        : in out Context;
      Item_S     : TREE;
      Is_Global  : Boolean;
      Export_All : Boolean;
      Has_Dyn    : out Boolean;
      Only_Item  : TREE := CONST_VOID)
   is
      L        : SEQ_TYPE := MAKE;
      Item     : TREE;
      Done_One : Boolean;
      Local_Insts : Pending_Inst_Vecs.Vector;
   begin
      Has_Dyn := False;
      if Is_Void_Tree (Item_S) and then Is_Void_Tree (Only_Item) then
         return;
      end if;

      -- Pass 1: register types, exceptions, generic specs/bodies, subprogram specs
      if Is_Void_Tree (Only_Item) then
         L := LIST (Item_S);
      end if;
      Done_One := False;
      while not Done_One and then (not Is_Void_Tree (Only_Item) or else not IS_EMPTY (L)) loop
         if not Is_Void_Tree (Only_Item) then
            Item := Only_Item;
            Done_One := True;
         else
            POP (L, Item);
         end if;
         case KIND (Item) is
            when DN_TYPE_DECL =>
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, Item);
                  TS  : constant TREE := D (SM_TYPE_SPEC, TID);
                  Dummy : Type_Info;
               begin
                  if not Is_Void_Tree (TS) then
                     Dummy := Get_Type_Info (Ctx, TS);
                     pragma Unreferenced (Dummy);
                  end if;
               end;

            when DN_SUBTYPE_DECL =>
               declare
                  SID : constant TREE := D (AS_SOURCE_NAME, Item);
                  TS  : constant TREE := D (SM_TYPE_SPEC, SID);
                  Dummy : Type_Info;
               begin
                  if not Is_Void_Tree (TS) then
                     Dummy := Get_Type_Info (Ctx, TS);
                     pragma Unreferenced (Dummy);
                  end if;
               end;

            when DN_EXCEPTION_DECL =>
               declare
                  NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Item));
                  EID : TREE;
               begin
                  while not IS_EMPTY (NS) loop
                     POP (NS, EID);
                     declare
                        Can      : constant TREE := Canonical_Def (EID);
                        Key      : constant Unsigned_32 := Tree_Key (Can);
                        QName    : constant String := Qualified_Ada_Name (Can);
                        In_Gen_B : constant Boolean :=
                          Length (Ctx.Gen_Inst_Tag) > 0 and then Ctx.In_Gen_Body and then not Is_Library_Level (Can);
                     begin
                        if (Is_Global and then Export_All and then Is_Library_Level (Can)) or else Ctx.Has_Stubs then
                           declare
                              Sym : constant String := "@__ada_exc__" & QName;
                              TI  : Type_Info;
                           begin
                              Add_Data_Decl (Ctx.E, Sym,
                                "export " & Sym & " = data align 1 { """ & Escape_Pir_String (Sym_Name (Can)) & "\0"" }");
                              TI.Pir_Type := To_Unbounded_String (Sym);
                              Ctx.Exc_Tags.Include (Key, TI);
                           end;
                        elsif In_Gen_B then
                           declare
                              Sym : constant String := "@__ada_exc_" & QName & "_" & Tree_Key_Str (Can) & To_String (Ctx.Gen_Inst_Tag);
                              TI  : Type_Info;
                           begin
                              Add_Data_Decl (Ctx.E, Sym,
                                Sym & " = data align 1 { """ & Escape_Pir_String (Sym_Name (Can)) & "\0"" }");
                              TI.Pir_Type := To_Unbounded_String (Sym);
                              Ctx.Gen_Local_Exc_Tags.Include (Key, TI);
                           end;
                        else
                           -- P3-5.10: Define local exception data directly so subunit-local exceptions don't hit Get_Exc_Tag extern fallback!
                           declare
                              Sym : constant String := "@__ada_exc_" & QName & "_" & Tree_Key_Str (Can);
                              TI  : Type_Info;
                           begin
                              Add_Data_Decl (Ctx.E, Sym,
                                Sym & " = data align 1 { """ & Escape_Pir_String (Sym_Name (Can)) & "\0"" }");
                              TI.Pir_Type := To_Unbounded_String (Sym);
                              Ctx.Exc_Tags.Include (Key, TI);
                           end;
                        end if;
                     end;
                  end loop;
               end;

            when DN_GENERIC_DECL =>
               declare
                  GID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
               begin
                  Ctx.Generic_Decls.Include (Tree_Key (GID), Item);
               end;

            when DN_SUBPROG_ENTRY_DECL =>
               declare
                  SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                  UK  : constant TREE := D (AS_UNIT_KIND, Item);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                     Ctx.Subp_Renames.Include (Tree_Key (SID), UK);
                  elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     Pre_Register_Subp (Ctx, SID, D (SM_SPEC, SID), Ctx.Cur_Subp_Key);
                  else
                     Pre_Register_Subp (Ctx, SID, D (AS_HEADER, Item), Ctx.Cur_Subp_Key,
                       Is_Subunit => (Ctx.In_Subunit or else Ctx.Has_Stubs));
                  end if;
               end;

            when DN_TASK_DECL =>
               declare
                  Src : constant TREE := D (AS_SOURCE_NAME, Item);
                  TS  : TREE := CONST_VOID;
               begin
                  if not Is_Void_Tree (Src) then
                     if KIND (Src) in CLASS_TYPE_NAME then
                        TS := D (SM_TYPE_SPEC, Src);
                     elsif KIND (Src) = DN_VARIABLE_ID then
                        TS := D (SM_OBJ_TYPE, Src);
                     end if;
                     if not Is_Void_Tree (TS) then
                        Register_Task_Spec (Ctx, TS, Ctx.Cur_Subp_Key, Force_Rebind => True);
                     end if;
                  end if;
               end;

            when DN_SUBPROGRAM_BODY =>
               declare
                  SID : constant TREE := Canonical_Def (Subst_Tree (Ctx, Canonical_Def (D (AS_SOURCE_NAME, Item))));
                  Bdy : constant TREE := D (AS_BODY, Item);
                  Is_Ext_Stub : constant Boolean :=
                    not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_STUB and then not Is_Gen_Subunit_Stub (Ctx, Item);
               begin
                  if KIND (SID) = DN_GENERIC_ID then
                     if not Ctx.Generic_Body.Contains (Tree_Key (SID))
                       or else Is_Void_Tree (Bdy)
                       or else KIND (Bdy) /= DN_STUB
                     then
                        Ctx.Generic_Body.Include (Tree_Key (SID), Item);
                     end if;
                     if not Ctx.Generic_Subst.Is_Empty then
                        Ctx.Generic_Env.Include (Tree_Key (SID), Ctx.Generic_Subst);
                     end if;
                     if Ctx.In_Gen_Body then
                        Ctx.Gen_Body_Nested_Gens.Append (Tree_Key (SID));
                     end if;
                  else
                     Pre_Register_Subp (Ctx, SID, D (AS_HEADER, Item), Ctx.Cur_Subp_Key,
                       Is_Subunit => (Ctx.In_Subunit or else Ctx.Has_Stubs or else Is_Ext_Stub));
                  end if;
               end;

            when DN_PACKAGE_BODY =>
               declare
                  PID : constant TREE := Canonical_Def (Subst_Tree (Ctx, Canonical_Def (D (AS_SOURCE_NAME, Item))));
                  Bdy : constant TREE := D (AS_BODY, Item);
               begin
                  if KIND (PID) = DN_GENERIC_ID then
                     if not Ctx.Generic_Body.Contains (Tree_Key (PID))
                       or else Is_Void_Tree (Bdy)
                       or else KIND (Bdy) /= DN_STUB
                     then
                        Ctx.Generic_Body.Include (Tree_Key (PID), Item);
                     end if;
                     if not Ctx.Generic_Subst.Is_Empty then
                        Ctx.Generic_Env.Include (Tree_Key (PID), Ctx.Generic_Subst);
                     end if;
                     if Ctx.In_Gen_Body then
                        Ctx.Gen_Body_Nested_Gens.Append (Tree_Key (PID));
                     end if;
                  end if;
               end;

            when DN_PACKAGE_DECL =>
               declare
                  PID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                  UK  : constant TREE := D (AS_UNIT_KIND, Item);
               begin
                  if Is_Void_Tree (UK) then
                     -- Nested package spec: if global, emit now; if local, only pre-register subps/tasks
                     -- here and emit its declarations in textual order in Pass 2!
                     declare
                        Hdr : constant TREE := D (AS_HEADER, Item);
                        procedure Pre_Reg_Pkg_Items (DS : TREE) is
                           PL : SEQ_TYPE;
                           PI : TREE;
                        begin
                           if Is_Void_Tree (DS) then return; end if;
                           PL := LIST (DS);
                           while not IS_EMPTY (PL) loop
                              POP (PL, PI);
                              if KIND (PI) = DN_SUBPROG_ENTRY_DECL then
                                 declare
                                    SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, PI));
                                    PUK : constant TREE := D (AS_UNIT_KIND, PI);
                                 begin
                                    if not Is_Void_Tree (PUK) and then KIND (PUK) = DN_RENAMES_UNIT then
                                       Ctx.Subp_Renames.Include (Tree_Key (SID), PUK);
                                    elsif not Is_Void_Tree (PUK) and then KIND (PUK) = DN_INSTANTIATION then
                                       Pre_Register_Subp (Ctx, SID, D (SM_SPEC, SID), Ctx.Cur_Subp_Key);
                                    elsif KIND (SID) /= DN_GENERIC_ID then
                                       Pre_Register_Subp (Ctx, SID, D (AS_HEADER, PI), Ctx.Cur_Subp_Key);
                                    end if;
                                 end;
                              elsif KIND (PI) = DN_TASK_DECL then
                                 declare
                                    Src : constant TREE := D (AS_SOURCE_NAME, PI);
                                    TS  : TREE := CONST_VOID;
                                 begin
                                    if not Is_Void_Tree (Src) then
                                       if KIND (Src) = DN_TYPE_ID then TS := D (SM_TYPE_SPEC, Src);
                                       elsif KIND (Src) = DN_VARIABLE_ID then TS := D (SM_OBJ_TYPE, Src);
                                       end if;
                                       if not Is_Void_Tree (TS) then
                                          Register_Task_Spec (Ctx, TS, Ctx.Cur_Subp_Key);
                                       end if;
                                    end if;
                                 end;
                              elsif KIND (PI) = DN_PACKAGE_DECL then
                                 declare
                                    PUK : constant TREE := D (AS_UNIT_KIND, PI);
                                    PH  : constant TREE :=
                                      (if Is_Void_Tree (PUK) then D (AS_HEADER, PI)
                                       elsif KIND (PUK) = DN_INSTANTIATION then D (SM_SPEC, Canonical_Def (D (AS_SOURCE_NAME, PI)))
                                       else CONST_VOID);
                                 begin
                                    if not Is_Void_Tree (PH) and then KIND (PH) = DN_PACKAGE_SPEC then
                                       Pre_Reg_Pkg_Items (D (AS_DECL_S1, PH));
                                       Pre_Reg_Pkg_Items (D (AS_DECL_S2, PH));
                                    end if;
                                 end;
                              end if;
                           end loop;
                        end Pre_Reg_Pkg_Items;
                     begin
                        if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                           Pre_Reg_Pkg_Items (D (AS_DECL_S1, Hdr));
                           Pre_Reg_Pkg_Items (D (AS_DECL_S2, Hdr));
                        end if;
                     end;
                  elsif KIND (UK) = DN_INSTANTIATION then
                     -- Emit RTL I/O package spec globals immediately in Pass 1 (before local subprograms use DEFAULT_BASE/WIDTH);
                     -- for user generic packages, pre-register subps/tasks here and emit ADecls_N + ISpec + body in Pass 2!
                     declare
                        ISpec    : constant TREE := D (SM_SPEC, PID);
                        Gen_Id   : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                        Gen_Name : constant String := (if not Is_Void_Tree (Gen_Id) then Sym_Name (Gen_Id) else "");
                        procedure Pre_Reg_Inst_Items (DS : TREE) is
                           PL : SEQ_TYPE;
                           PI : TREE;
                        begin
                           if Is_Void_Tree (DS) then return; end if;
                           PL := LIST (DS);
                           while not IS_EMPTY (PL) loop
                              POP (PL, PI);
                              if KIND (PI) = DN_SUBPROG_ENTRY_DECL then
                                 declare
                                    SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, PI));
                                    PUK : constant TREE := D (AS_UNIT_KIND, PI);
                                 begin
                                    if not Is_Void_Tree (PUK) and then KIND (PUK) = DN_RENAMES_UNIT then
                                       Ctx.Subp_Renames.Include (Tree_Key (SID), PUK);
                                    elsif not Is_Void_Tree (PUK) and then KIND (PUK) = DN_INSTANTIATION then
                                       Pre_Register_Subp (Ctx, SID, D (SM_SPEC, SID), Ctx.Cur_Subp_Key);
                                    elsif KIND (SID) /= DN_GENERIC_ID then
                                       Pre_Register_Subp (Ctx, SID, D (AS_HEADER, PI), Ctx.Cur_Subp_Key);
                                    end if;
                                 end;
                              elsif KIND (PI) = DN_TASK_DECL then
                                 declare
                                    Src : constant TREE := D (AS_SOURCE_NAME, PI);
                                    TS  : TREE := CONST_VOID;
                                 begin
                                    if not Is_Void_Tree (Src) then
                                       if KIND (Src) = DN_TYPE_ID then TS := D (SM_TYPE_SPEC, Src);
                                       elsif KIND (Src) = DN_VARIABLE_ID then TS := D (SM_OBJ_TYPE, Src);
                                       end if;
                                       if not Is_Void_Tree (TS) then
                                          Register_Task_Spec (Ctx, TS, Ctx.Cur_Subp_Key);
                                       end if;
                                    end if;
                                 end;
                              elsif KIND (PI) = DN_PACKAGE_DECL then
                                 declare
                                    PUK : constant TREE := D (AS_UNIT_KIND, PI);
                                    PH  : constant TREE :=
                                      (if Is_Void_Tree (PUK) then D (AS_HEADER, PI)
                                       elsif KIND (PUK) = DN_INSTANTIATION then D (SM_SPEC, Canonical_Def (D (AS_SOURCE_NAME, PI)))
                                       else CONST_VOID);
                                 begin
                                    if not Is_Void_Tree (PUK) and then KIND (PUK) = DN_INSTANTIATION then
                                       Pre_Reg_Inst_Items (D (SM_DECL_S, PUK));
                                    end if;
                                    if not Is_Void_Tree (PH) and then KIND (PH) = DN_PACKAGE_SPEC then
                                       Pre_Reg_Inst_Items (D (AS_DECL_S1, PH));
                                       Pre_Reg_Inst_Items (D (AS_DECL_S2, PH));
                                    end if;
                                 end;
                              end if;
                           end loop;
                        end Pre_Reg_Inst_Items;
                     begin
                        if not Is_Void_Tree (ISpec) and then KIND (ISpec) = DN_PACKAGE_SPEC then
                           if Is_RTL_Generic (Gen_Id) then
                              -- P3-5.7: Emit global RTL I/O spec vars in Pass 1; local TEXT_IO.*_IO emitted in Pass 2 as stack locals!
                              if Is_Global then
                                 declare
                                    Sub_Dyn : Boolean;
                                 begin
                                    Emit_Decl_Items (Ctx, D (AS_DECL_S1, ISpec), Is_Global => True, Export_All => Export_All, Has_Dyn => Sub_Dyn);
                                    Emit_Decl_Items (Ctx, D (AS_DECL_S2, ISpec), Is_Global => True, Export_All => False, Has_Dyn => Sub_Dyn);
                                 end;
                              end if;
                           else
                              Pre_Reg_Inst_Items (D (SM_DECL_S, UK));
                              Pre_Reg_Inst_Items (D (AS_DECL_S1, ISpec));
                              Pre_Reg_Inst_Items (D (AS_DECL_S2, ISpec));
                           end if;
                        end if;
                     end;
                  end if;
               end;

            when others =>
               null;
         end case;
      end loop;

      -- Pass 2: emit variables, constants, renamings, nested package specs/bodies, subprogram bodies, generic instances
      if Is_Void_Tree (Only_Item) then
         L := LIST (Item_S);
      end if;
      Done_One := False;
      while not Done_One and then (not Is_Void_Tree (Only_Item) or else not IS_EMPTY (L)) loop
         if not Is_Void_Tree (Only_Item) then
            Item := Only_Item;
            Done_One := True;
         else
            POP (L, Item);
         end if;
         case KIND (Item) is
            when DN_TYPE_DECL | DN_SUBTYPE_DECL =>
               -- P3-3.14: Skip incomplete type stub declarations (`type T;`) to avoid premature & duplicate elaboration!
               if KIND (Item) = DN_TYPE_DECL and then Is_Void_Tree (D (AS_TYPE_DEF, Item)) then
                  null;
               else
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, Item);
               begin
                  if not Is_Void_Tree (TID) and then KIND (TID) in CLASS_TYPE_NAME then
                     declare
                        TS : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TID)));
                        SI_D : constant TREE :=
                          (if KIND (Item) = DN_SUBTYPE_DECL then D (AS_SUBTYPE_INDICATION, Item)
                           elsif KIND (Item) = DN_TYPE_DECL and then not Is_Void_Tree (D (AS_TYPE_DEF, Item))
                             and then KIND (D (AS_TYPE_DEF, Item)) in DN_DERIVED_DEF | DN_ACCESS_DEF
                           then D (AS_SUBTYPE_INDICATION, D (AS_TYPE_DEF, Item))
                           else CONST_VOID);
                        Mark_T : constant TREE :=
                          (if not Is_Void_Tree (SI_D) and then KIND (SI_D) = DN_SUBTYPE_INDICATION
                             and then not Is_Void_Tree (D (AS_NAME, SI_D))
                             and then not Is_Void_Tree (Get_Name_Defn (D (AS_NAME, SI_D)))
                             and then KIND (Get_Name_Defn (D (AS_NAME, SI_D))) in CLASS_TYPE_NAME
                           then D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, SI_D)))
                           else CONST_VOID);
                        Has_Real_C : constant Boolean :=
                          not Is_Void_Tree (SI_D) and then KIND (SI_D) = DN_SUBTYPE_INDICATION
                          and then not Is_Void_Tree (D (AS_CONSTRAINT, SI_D))
                          and then KIND (D (AS_CONSTRAINT, SI_D)) in DN_FLOAT_CONSTRAINT | DN_FIXED_CONSTRAINT;
                     begin
                        Emit_Type_Elab_Checks (Ctx, TS, Mark_Type => Mark_T, Has_Real_Constraint => Has_Real_C);
                        -- P3-3.19: Propagate parent Saved_Type_Ranges/Saved_Type_Discs for derived/unconstrained child subtypes!
                        if not Is_Void_Tree (TS) and then not Is_Void_Tree (Mark_T) then
                           declare
                              MT_R : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Mark_T));
                              Has_New_C : constant Boolean :=
                                not Is_Void_Tree (SI_D) and then KIND (SI_D) = DN_SUBTYPE_INDICATION
                                and then not Is_Void_Tree (D (AS_CONSTRAINT, SI_D));
                           begin
                              if not Has_New_C and then not Is_Void_Tree (MT_R) then
                                 if not Ctx.Saved_Type_Ranges.Contains (Tree_Key (TS))
                                   and then Ctx.Saved_Type_Ranges.Contains (Tree_Key (MT_R))
                                 then
                                    Ctx.Saved_Type_Ranges.Include (Tree_Key (TS), Ctx.Saved_Type_Ranges.Element (Tree_Key (MT_R)));
                                 end if;
                                 if not Ctx.Saved_Type_Discs.Contains (Tree_Key (TS))
                                   and then Ctx.Saved_Type_Discs.Contains (Tree_Key (MT_R))
                                 then
                                    Ctx.Saved_Type_Discs.Include (Tree_Key (TS), Ctx.Saved_Type_Discs.Element (Tree_Key (MT_R)));
                                 end if;
                              end if;
                           end;
                        end if;
                        -- LRM 3.6.1 (c35003a/b, c36104a/b, c36105b): check explicit discrete subtype ranges on constrained array / derived / subtype defs
                        if Ctx.Enable_Checks then
                           declare
                              IC_Host : TREE := CONST_VOID;
                           begin
                              if KIND (Item) = DN_TYPE_DECL and then not Is_Void_Tree (D (AS_TYPE_DEF, Item))
                                and then KIND (D (AS_TYPE_DEF, Item)) = DN_CONSTRAINED_ARRAY_DEF
                              then
                                 IC_Host := D (AS_TYPE_DEF, Item);
                              elsif not Is_Void_Tree (SI_D) and then KIND (SI_D) = DN_SUBTYPE_INDICATION then
                                 IC_Host := SI_D;
                              end if;
                              if not Is_Void_Tree (IC_Host)
                                and then not Is_Void_Tree (D (AS_CONSTRAINT, IC_Host))
                                and then KIND (D (AS_CONSTRAINT, IC_Host)) = DN_INDEX_CONSTRAINT
                                and then not Is_Void_Tree (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, IC_Host)))
                              then
                                 declare
                                    DSL : SEQ_TYPE := LIST (D (AS_DISCRETE_RANGE_S, D (AS_CONSTRAINT, IC_Host)));
                                    DSI : TREE;
                                    PT_U, Lo_U, Hi_U : Unbounded_String;
                                    pragma Unreferenced (PT_U, Lo_U, Hi_U);
                                 begin
                                    while not IS_EMPTY (DSL) loop
                                       POP (DSL, DSI);
                                       if KIND (IC_Host) = DN_CONSTRAINED_ARRAY_DEF
                                         or else KIND (DSI) in DN_DISCRETE_SUBTYPE | DN_RANGE_ATTRIBUTE
                                       then
                                          Emit_Discrete_Range (Ctx, DSI, PT_U, Lo_U, Hi_U);
                                       end if;
                                    end loop;
                                 end;
                              end if;
                           end;
                        end if;
                        -- Record declaring master for access-to-task-containing types (LRM 9.4)
                        if KIND (Item) = DN_TYPE_DECL and then not Is_Void_Tree (TS)
                          and then KIND (TS) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                          and then Length (Ctx.Cur_Master_Reg) > 0
                          and then Type_Contains_Task (Ctx, D (SM_DESIG_TYPE, TS))
                        then
                           declare
                              BS  : constant TREE := Base_Type_Spec (TS);
                              MTI : Type_Info;
                           begin
                              -- Do not overwrite root access type's master when declaring a derived access type
                              if KIND (BS) = DN_ACCESS and then Is_Void_Tree (D (SM_DERIVED, BS)) then
                                 if not Is_Global and then Ctx.Cur_Subp_Key /= 0 then
                                    -- Task/subprogram-local access type: stack-local alloca so concurrent task instances
                                    -- don't clobber each other (c9a005a/7a), plus global mirror for nested subprograms
                                    -- and generic instantiations (c94011a).
                                    declare
                                       MA     : constant String := Next_Reg (Ctx.E, "amstl");
                                       MSym   : constant String := "@ada_accmst_" & Tree_Key_Str (BS) & To_String (Ctx.Gen_Inst_Tag);
                                       MG     : constant String := Next_Reg (Ctx.E, "amstg");
                                       Cap_Sl : Natural := 0;
                                    begin
                                       Emit_Entry_Alloca (Ctx.E, MA & " = alloca ptr, align 8");
                                       Emit_Inst (Ctx.E, "store.ptr " & To_String (Ctx.Cur_Master_Reg) & ", " & MA & ", align 8");
                                       if Ctx.Vars.Contains (Tree_Key (BS))
                                         and then Ctx.Vars.Element (Tree_Key (BS)).Owner_Subp = Ctx.Cur_Subp_Key
                                       then
                                          Cap_Sl := Ctx.Vars.Element (Tree_Key (BS)).Captured_Slot;
                                       end if;
                                       if Cap_Sl > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                                          declare
                                             FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                             FP  : constant String := Next_Reg (Ctx.E, "amfp");
                                          begin
                                             Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (Cap_Sl));
                                             Emit_Inst (Ctx.E, "store.ptr " & MA & ", " & FP & ", align 8");
                                          end;
                                       end if;
                                       Add_Global_Decl (Ctx.E, MSym, MSym & " = global ptr = zeroinit, align 8");
                                       Emit_Inst (Ctx.E, MG & " = addr " & MSym);
                                       Emit_Inst (Ctx.E, "store.ptr " & To_String (Ctx.Cur_Master_Reg) & ", " & MG & ", align 8");
                                       MTI.Pir_Type := To_Unbounded_String (MA & "|" & MSym);
                                       MTI.Dims (1).First := Integer_64 (Ctx.Cur_Subp_Key);
                                       MTI.Dims (1).Last  := Integer_64 (Cap_Sl);
                                       Ctx.Access_Masters.Include (Tree_Key (BS), MTI);
                                    end;
                                 else
                                    declare
                                       MSym : constant String := "@ada_accmst_" & Tree_Key_Str (BS) & To_String (Ctx.Gen_Inst_Tag);
                                       MA   : constant String := Next_Reg (Ctx.E, "amst");
                                    begin
                                       Add_Global_Decl (Ctx.E, MSym, MSym & " = global ptr = zeroinit, align 8");
                                       Emit_Inst (Ctx.E, MA & " = addr " & MSym);
                                       Emit_Inst (Ctx.E, "store.ptr " & To_String (Ctx.Cur_Master_Reg) & ", " & MA & ", align 8");
                                       MTI.Pir_Type := To_Unbounded_String (MSym);
                                       Ctx.Access_Masters.Include (Tree_Key (BS), MTI);
                                    end;
                                 end if;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;
               end if;

            when DN_VARIABLE_DECL | DN_CONSTANT_DECL =>
               Emit_Object_Decl (Ctx, Item, Is_Global, Export_All, Has_Dyn);

            when DN_RENAMES_OBJ_DECL =>
               declare
                  Src_N  : constant TREE := D (AS_SOURCE_NAME, Item);
                  RID    : constant TREE := Canonical_Def (Src_N);
                  RK     : constant Unsigned_32 := Tree_Key (RID);
                  Init_N : constant TREE := (if not Is_Void_Tree (Src_N) then D (SM_INIT_EXP, Src_N) else CONST_VOID);
                  Expr_N : constant TREE := (if not Is_Void_Tree (Init_N) then Init_N else D (AS_NAME, Item));
                  Is_BF_Ren : Boolean := False;
                  BF_FE_R   : Field_Entry;
                  BF_Rec_P  : Unbounded_String;
                  BF_Comp_T : TREE := CONST_VOID;
                  LV     : Expr_Val;
                  V      : Var_Info;
               begin
                  if Ctx.Vars.Contains (RK) and then Ctx.Vars.Element (RK).Owner_Subp = Ctx.Cur_Subp_Key then
                     V.Captured_Slot     := Ctx.Vars.Element (RK).Captured_Slot;
                     V.Con_Captured_Slot := Ctx.Vars.Element (RK).Con_Captured_Slot;
                  end if;
                  -- P3-3.16: Detect renaming of a bitfield record component (or chaining a bitfield renaming)!
                  if not Is_Void_Tree (Expr_N) and then KIND (Expr_N) = DN_USED_OBJECT_ID then
                     declare
                        ED : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Expr_N)));
                     begin
                        if not Is_Void_Tree (ED) and then Ctx.Vars.Contains (Tree_Key (ED)) then
                           declare
                              EV : constant Var_Info := Ctx.Vars.Element (Tree_Key (ED));
                           begin
                              if EV.Is_Shared or else (KIND (ED) = DN_VARIABLE_ID and then DB (SM_IS_SHARED, ED)) then
                                 V.Is_Shared := True;
                              end if;
                              if EV.Is_Bitfield then
                                 Is_BF_Ren := True;
                                 BF_FE_R   := EV.BF_FE;
                                 BF_Rec_P  := To_Unbounded_String (Emit_Var_Addr (Ctx, EV));
                                 BF_Comp_T := EV.Ada_Type;
                              end if;
                           end;
                        elsif not Is_Void_Tree (ED) and then KIND (ED) = DN_VARIABLE_ID and then DB (SM_IS_SHARED, ED) then
                           V.Is_Shared := True;
                        end if;
                     end;
                  elsif not Is_Void_Tree (Expr_N) and then KIND (Expr_N) = DN_SELECTED then
                     declare
                        Des_R : TREE := D (AS_DESIGNATOR, Expr_N);
                        Def_R : TREE;
                     begin
                        while not Is_Void_Tree (Des_R) and then KIND (Des_R) = DN_SELECTED loop
                           Des_R := D (AS_DESIGNATOR, Des_R);
                        end loop;
                        Def_R := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des_R)));
                        if not Is_Void_Tree (Def_R) and then KIND (Def_R) in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                           declare
                              Pfx_E  : constant TREE := D (AS_NAME, Expr_N);
                              Pfx_Ts : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                              Rec_Ts : constant TREE :=
                                (if Is_Access_Type (Pfx_Ts) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_Ts)))
                                 else Pfx_Ts);
                              Dummy_RTI : constant Type_Info := Get_Type_Info (Ctx, Rec_Ts);
                              pragma Unreferenced (Dummy_RTI);
                              CK : constant Unsigned_32 := Tree_Key (Def_R);
                           begin
                              if Ctx.Fields.Contains (CK) and then Is_Bitfield (Ctx.Fields.Element (CK)) then
                                 declare
                                    PLV : constant Expr_Val := Emit_LValue (Ctx, Pfx_E);
                                    PT_P : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, PLV.Ada_Type));
                                 begin
                                    Is_BF_Ren := True;
                                    BF_FE_R   := Ctx.Fields.Element (CK);
                                    BF_Comp_T := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def_R));
                                    BF_Rec_P  := PLV.Reg;
                                    if Is_Access_Type (PT_P) then
                                       if PLV.Kind = EK_Addr then
                                          declare
                                             LP : constant String := Next_Reg (Ctx.E, "bfrp");
                                          begin
                                             Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (BF_Rec_P) & ", align 8");
                                             BF_Rec_P := To_Unbounded_String (LP);
                                          end;
                                       end if;
                                       Emit_Null_Check (Ctx, To_String (BF_Rec_P));
                                    end if;
                                 end;
                              end if;
                           end;
                        elsif not Is_Void_Tree (Def_R) and then KIND (Def_R) = DN_VARIABLE_ID and then DB (SM_IS_SHARED, Def_R) then
                           V.Is_Shared := True;
                        end if;
                     end;
                  end if;
                  if Is_BF_Ren then
                     declare
                        BTI : constant Type_Info := Get_Type_Info (Ctx, BF_Comp_T);
                     begin
                        LV.Kind     := EK_Addr;
                        LV.Reg      := BF_Rec_P;
                        LV.Ada_Type := BF_Comp_T;
                        LV.Pir_Type := BTI.Pir_Type;
                        LV.Align    := BTI.Align;
                        V.Is_Bitfield := True;
                        V.BF_FE       := BF_FE_R;
                     end;
                  else
                     LV := Emit_LValue (Ctx, Expr_N);
                  end if;
                  V.Ada_Type   := LV.Ada_Type;
                  V.Pir_Type   := LV.Pir_Type;
                  V.Align      := LV.Align;
                  V.Owner_Subp := (if Is_Global then 0 else Ctx.Cur_Subp_Key);
                  if Is_Global then
                     declare
                        Is_Exp : constant Boolean := Export_All and then Is_Library_Level (RID);
                        Link   : constant String := (if Is_Exp then "export " else "");
                        QName  : constant String := Qualified_Ada_Name (RID);
                        Sfx    : constant String := Tree_Key_Str (RID) & To_String (Ctx.Gen_Inst_Tag);
                     begin
                        if LV.Kind = EK_Slice then
                           declare
                              GSym : constant String := (if Is_Exp then "@ada_rnd__" & QName else "@ada_rnd_" & Sfx);
                              GA   : constant String := Next_Reg (Ctx.E, "rndg");
                              P0   : constant String := Next_Reg (Ctx.E, "rnd0");
                           begin
                              Add_Global_Decl (Ctx.E, GSym, Link & GSym & " = global @__ada_arr_desc = zeroinit, align 8");
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & GA & ", 0");
                              Emit_Inst (Ctx.E, "store.ptr " & To_String (LV.Reg) & ", " & P0 & ", align 8");
                              for D_I in 1 .. Natural'Min (LV.NDims, Max_Array_Dims) loop
                                 declare
                                    FF : constant String := Next_Reg (Ctx.E, "rndf");
                                    LF : constant String := Next_Reg (Ctx.E, "rndl");
                                 begin
                                    Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & GA & ", " & Img (2 * D_I - 1));
                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (LV.First_Regs (D_I)) & ", " & FF & ", align 4");
                                    Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & GA & ", " & Img (2 * D_I));
                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (LV.Last_Regs (D_I)) & ", " & LF & ", align 4");
                                 end;
                              end loop;
                              V.Kind        := VK_Dyn_Array;
                              V.Pir_Name    := LV.Reg;
                              V.NDims       := LV.NDims;
                              V.First_Regs  := LV.First_Regs;
                              V.Last_Regs   := LV.Last_Regs;
                              V.Has_Desc    := True;
                              V.Desc_Global := To_Unbounded_String (GSym);
                           end;
                        elsif LV.Kind = EK_Scalar then
                           declare
                              GSym : constant String := "@ada_rns_" & Sfx;
                              GA   : constant String := Next_Reg (Ctx.E, "rnsg");
                           begin
                              Add_Global_Decl (Ctx.E, GSym,
                                GSym & " = global " & To_String (LV.Pir_Type) & " = zeroinit, align " & Img (Natural'Max (1, LV.Align)));
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store." & To_String (LV.Pir_Type) & " " & To_String (LV.Reg) & ", " & GA & ", align " & Img (Natural'Max (1, LV.Align)));
                              if Is_Exp then
                                 declare
                                    PSym : constant String := "@ada_rnp__" & QName;
                                    PA   : constant String := Next_Reg (Ctx.E, "rnpa");
                                 begin
                                    Add_Global_Decl (Ctx.E, PSym, "export " & PSym & " = global ptr = zeroinit, align 8");
                                    Emit_Inst (Ctx.E, PA & " = addr " & PSym);
                                    Emit_Inst (Ctx.E, "store.ptr " & GA & ", " & PA & ", align 8");
                                 end;
                              end if;
                              V.Kind     := VK_Global;
                              V.Pir_Name := To_Unbounded_String (GSym);
                           end;
                        else
                           declare
                              GSym : constant String := (if Is_Exp then "@ada_rnp__" & QName else "@ada_rnp_" & Sfx);
                              GA   : constant String := Next_Reg (Ctx.E, "rnpg");
                           begin
                              Add_Global_Decl (Ctx.E, GSym, Link & GSym & " = global ptr = zeroinit, align 8");
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store.ptr " & To_String (LV.Reg) & ", " & GA & ", align 8");
                              V.Kind     := VK_Global_Ptr;
                              V.Pir_Name := To_Unbounded_String (GSym);
                           end;
                        end if;
                        -- P3-3.3: Track constrainedness for global record renamings!
                        if Get_Type_Info (Ctx, V.Ada_Type).Kind = TK_Record then
                           declare
                              Con_R : constant String := Emit_Actual_Constrained (Ctx, Expr_N);
                              CSym  : constant String := (if Is_Exp then "@ada_rnc__" & QName else "@ada_rnc_" & Sfx);
                              CA    : constant String := Next_Reg (Ctx.E, "rncg");
                           begin
                              Add_Global_Decl (Ctx.E, CSym, Link & CSym & " = global i8 = 1, align 1");
                              Emit_Inst (Ctx.E, CA & " = addr " & CSym);
                              Emit_Inst (Ctx.E, "store.i8 " & Con_R & ", " & CA & ", align 1");
                              V.Con_Global := To_Unbounded_String (CSym);
                           end;
                        end if;
                     end;
                  elsif LV.Kind = EK_Slice then
                     V.Kind       := VK_Dyn_Array;
                     V.Pir_Name   := LV.Reg;
                     V.NDims      := LV.NDims;
                     V.First_Regs := LV.First_Regs;
                     V.Last_Regs  := LV.Last_Regs;
                     if V.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                        declare
                           Desc_R : constant String := Next_Reg (Ctx.E, "rnd");
                           FP_R   : constant String := Next_Reg (Ctx.E, "rnp");
                           FSI    : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                           P0_R   : constant String := Next_Reg (Ctx.E, "rn0");
                        begin
                           Emit_Entry_Alloca (Ctx.E, Desc_R & " = alloca @__ada_arr_desc, align 8");
                           Emit_Inst (Ctx.E, P0_R & " = field_ptr @__ada_arr_desc, " & Desc_R & ", 0");
                           Emit_Inst (Ctx.E, "store.ptr " & To_String (LV.Reg) & ", " & P0_R & ", align 8");
                           for D_I in 1 .. Natural'Min (LV.NDims, Max_Array_Dims) loop
                              declare
                                 FF : constant String := Next_Reg (Ctx.E, "rnf");
                                 LF : constant String := Next_Reg (Ctx.E, "rnl");
                              begin
                                 Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & Desc_R & ", " & Img (2 * D_I - 1));
                                 Emit_Inst (Ctx.E, "store.i32 " & To_String (LV.First_Regs (D_I)) & ", " & FF & ", align 4");
                                 Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & Desc_R & ", " & Img (2 * D_I));
                                 Emit_Inst (Ctx.E, "store.i32 " & To_String (LV.Last_Regs (D_I)) & ", " & LF & ", align 4");
                              end;
                           end loop;
                           Emit_Inst (Ctx.E, FP_R & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (V.Captured_Slot));
                           Emit_Inst (Ctx.E, "store.ptr " & Desc_R & ", " & FP_R & ", align 8");
                           V.Has_Desc := True;
                        end;
                     end if;
                  elsif LV.Kind = EK_Scalar then
                     if V.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                        declare
                           Sp  : constant String := Next_Reg (Ctx.E, "rns");
                           FP  : constant String := Next_Reg (Ctx.E, "rnp");
                           FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                           TI  : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
                        begin
                           Emit_Entry_Alloca (Ctx.E, Sp & " = alloca " & Img (Natural'Max (1, TI.Byte_Size)) & ", align " & Img (Natural'Max (1, TI.Align)));
                           Emit_Inst (Ctx.E, "store." & To_String (LV.Pir_Type) & " " & To_String (LV.Reg) & ", " & Sp & ", align " & Img (Natural'Max (1, TI.Align)));
                           Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (V.Captured_Slot));
                           Emit_Inst (Ctx.E, "store.ptr " & Sp & ", " & FP & ", align 8");
                           V.Kind     := VK_Local_Alloca;
                           V.Pir_Name := To_Unbounded_String (Sp);
                        end;
                     else
                        V.Kind     := VK_Param_Val;
                        V.Pir_Name := LV.Reg;
                     end if;
                  else
                     V.Kind     := VK_Param_Ptr;
                     V.Pir_Name := LV.Reg;
                     if V.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                        declare
                           FP  : constant String := Next_Reg (Ctx.E, "rnp");
                           FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                        begin
                           Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (V.Captured_Slot));
                           Emit_Inst (Ctx.E, "store.ptr " & To_String (LV.Reg) & ", " & FP & ", align 8");
                        end;
                     end if;
                     -- P3-3.3: Populate V.Constrained_Reg (and spill into frame if captured!) for local record renamings
                     if Get_Type_Info (Ctx, V.Ada_Type).Kind = TK_Record then
                        declare
                           Con_R : constant String := Emit_Actual_Constrained (Ctx, Expr_N);
                        begin
                           V.Constrained_Reg := To_Unbounded_String (Con_R);
                           if V.Con_Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                              declare
                                 CSp : constant String := Next_Reg (Ctx.E, "rncs");
                                 CFP : constant String := Next_Reg (Ctx.E, "rncf");
                                 FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                              begin
                                 Emit_Entry_Alloca (Ctx.E, CSp & " = alloca i8, align 1");
                                 Emit_Inst (Ctx.E, "store.i8 " & Con_R & ", " & CSp & ", align 1");
                                 Emit_Inst (Ctx.E, CFP & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (V.Con_Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & CSp & ", " & CFP & ", align 8");
                              end;
                           end if;
                        end;
                     end if;
                  end if;
                  Ctx.Vars.Include (RK, V);
               end;

            when DN_ADDRESS =>
               -- Deferred object address clause elaboration (`for X use at Y'Address;` where Y is declared after X)
               declare
                  Nm_N  : constant TREE := D (AS_NAME, Item);
                  Exp_N : constant TREE := D (AS_EXP, Item);
                  Def_N : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Nm_N)));
               begin
                  if not Is_Void_Tree (Def_N) and then not Is_Void_Tree (Exp_N)
                    and then KIND (Def_N) in DN_VARIABLE_ID | DN_CONSTANT_ID
                    and then Ctx.Vars.Contains (Tree_Key (Def_N))
                  then
                     declare
                        V : Var_Info := Ctx.Vars.Element (Tree_Key (Def_N));
                     begin
                        if (V.Kind = VK_Local_Ptr or else V.Kind = VK_Global_Ptr) and then V.Has_Desc then
                           declare
                              AV   : constant Expr_Val := Emit_Expr (Ctx, Exp_N);
                              A64  : constant String := Emit_Int_Convert (Ctx, To_String (AV.Reg), To_String (AV.Pir_Type), "i64");
                              PtrR : constant String := Next_Reg (Ctx.E, "daptr");
                              Init_E : constant TREE := D (SM_INIT_EXP, Def_N);
                              TI_D   : constant Type_Info := Get_Type_Info (Ctx, V.Ada_Type);
                           begin
                              Emit_Inst (Ctx.E, PtrR & " = inttoptr.ptr " & A64);
                              if V.Kind = VK_Global_Ptr then
                                 declare
                                    GA : constant String := Next_Reg (Ctx.E, "dgpa");
                                 begin
                                    Emit_Inst (Ctx.E, GA & " = addr " & To_String (V.Pir_Name));
                                    Emit_Inst (Ctx.E, "store.ptr " & PtrR & ", " & GA & ", align 8");
                                 end;
                              else
                                 Emit_Inst (Ctx.E, "store.ptr " & PtrR & ", " & To_String (V.Pir_Name) & ", align 8");
                              end if;
                              V.Has_Desc := False;
                              Ctx.Vars.Replace (Tree_Key (Def_N), V);
                              -- Also run deferred Init_Exp or Default_Init now that the address clause pointer is bound (LRM 13.5, P3-6.10)
                              if not Is_Void_Tree (Init_E) then
                                 if TI_D.Kind = TK_Scalar then
                                    declare
                                       IV : constant Expr_Val := Emit_Expr (Ctx, Init_E, V.Ada_Type);
                                       VR : constant String := Emit_Scalar_Coerce (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), To_String (TI_D.Pir_Type));
                                    begin
                                       Emit_Range_Check (Ctx, VR, To_String (TI_D.Pir_Type), V.Ada_Type);
                                       Emit_Discriminant_Check (Ctx, VR, V.Ada_Type, Is_Access => True);
                                       Emit_Inst (Ctx.E, "store." & To_String (TI_D.Pir_Type) & " " & VR & ", " & PtrR & ", align " & Img (TI_D.Align));
                                    end;
                                 else
                                    Emit_Init_Composite (Ctx, PtrR, V.Ada_Type, Init_E);
                                 end if;
                              else
                                 Emit_Default_Init (Ctx, PtrR, V.Ada_Type);
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;

            when DN_LENGTH_ENUM_REP =>
               -- LRM 13.2(8) (cd1c06a): evaluate 'STORAGE_SIZE expression ONCE when the length clause is elaborated!
               declare
                  Attr_N : constant TREE := D (AS_NAME, Item);
                  Exp_N  : constant TREE := D (AS_EXP, Item);
               begin
                  if not Is_Void_Tree (Attr_N) and then KIND (Attr_N) = DN_ATTRIBUTE
                    and then not Is_Void_Tree (Exp_N)
                  then
                     declare
                        AD : constant TREE := D (AS_USED_NAME_ID, Attr_N);
                        AN : constant String := (if not Is_Void_Tree (AD) then Sym_Name (AD) else "");
                     begin
                        if AN = "STORAGE_SIZE" then
                           -- P3-6.6: Evaluate 'STORAGE_SIZE once; export for library types and capture in frame for local types!
                           declare
                              Pfx_P : constant TREE := D (AS_NAME, Attr_N);
                              PDef  : constant TREE := (if not Is_Void_Tree (Pfx_P) then Get_Name_Defn (Pfx_P) else CONST_VOID);
                              PTS   : constant TREE :=
                                (if not Is_Void_Tree (PDef) and then KIND (PDef) in CLASS_TYPE_NAME
                                 then Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, PDef))))
                                 else CONST_VOID);
                              Cur_N : constant TREE := (if not Is_Void_Tree (PTS) then D (XD_SOURCE_NAME, PTS) else CONST_VOID);
                              Is_Lib_T : constant Boolean := not Is_Void_Tree (Cur_N) and then Is_Library_Level (Cur_N);
                           begin
                           if not Ctx.Saved_Expr_Vals.Contains (Tree_Key (Exp_N)) then
                              declare
                                 SV : constant Expr_Val := Emit_Expr (Ctx, Exp_N);
                                 SE : Saved_Expr_Info;
                              begin
                                 if SV.Kind = EK_Scalar then
                                    if Is_Lib_T then
                                       declare
                                          SSym : constant String := "@ada_ssz__" & Qualified_Ada_Name (Cur_N);
                                          Link_S : constant String := (if Export_All then "export " else "");
                                          V64  : constant String := Emit_Int_Convert (Ctx, To_String (SV.Reg), To_String (SV.Pir_Type), "i64");
                                          SA   : constant String := Next_Reg (Ctx.E, "ssza");
                                       begin
                                          Add_Global_Decl (Ctx.E, SSym, Link_S & SSym & " = global i64 = 0, align 8");
                                          Emit_Inst (Ctx.E, SA & " = addr " & SSym);
                                          Emit_Inst (Ctx.E, "store.i64 " & V64 & ", " & SA & ", align 8");
                                          SE.Ptr_Reg    := To_Unbounded_String (SSym);
                                          SE.Is_Global  := True;
                                          SE.Pir_Type   := To_Unbounded_String ("i64");
                                          SE.Ada_Type   := SV.Ada_Type;
                                          SE.Align      := 8;
                                          SE.Owner_Subp := 0;
                                       end;
                                    else
                                       Save_Scalar_Slot (Ctx, To_String (SV.Reg), To_String (SV.Pir_Type), SV.Align, SE.Ptr_Reg, SE.Is_Global);
                                       SE.Pir_Type   := SV.Pir_Type;
                                       SE.Ada_Type   := SV.Ada_Type;
                                       SE.Align      := SV.Align;
                                       SE.Owner_Subp := Ctx.Cur_Subp_Key;
                                       -- Capture local 'STORAGE_SIZE slot into parent static-link frame for nested subprograms/tasks
                                       if not SE.Is_Global and then Ctx.Cur_Subp_Key /= 0 and then Length (Ctx.Cur_Frame_Reg) > 0
                                         and then Ctx.Vars.Contains (Tree_Key (Exp_N))
                                         and then Ctx.Vars.Element (Tree_Key (Exp_N)).Owner_Subp = Ctx.Cur_Subp_Key
                                         and then Ctx.Vars.Element (Tree_Key (Exp_N)).Captured_Slot > 0
                                       then
                                          declare
                                             SI_F : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                             CS   : constant Natural := Ctx.Vars.Element (Tree_Key (Exp_N)).Captured_Slot;
                                             FP_S : constant String := Next_Reg (Ctx.E, "sscf");
                                          begin
                                             Emit_Inst (Ctx.E, FP_S & " = field_ptr " & To_String (SI_F.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (CS));
                                             Emit_Inst (Ctx.E, "store.ptr " & To_String (SE.Ptr_Reg) & ", " & FP_S & ", align 8");
                                             SE.Cap_Slot := CS;
                                          end;
                                       end if;
                                    end if;
                                    Ctx.Saved_Expr_Vals.Include (Tree_Key (Exp_N), SE);
                                 end if;
                              end;
                           end if;
                           -- P3-3.15: Export library-level pool counter and reset local pool counter to 0 on elaboration!
                           if not Is_Void_Tree (PTS) and then KIND (PTS) = DN_ACCESS then
                              declare
                                 PSym   : constant String :=
                                   (if Is_Lib_T then "@ada_pool_used__" & Qualified_Ada_Name (Cur_N)
                                    else "@ada_pool_used_" & Tree_Key_Str (PTS) & To_String (Ctx.Gen_Inst_Tag));
                                 Link_P : constant String := (if Is_Lib_T and then Export_All then "export " else "");
                                 PA     : constant String := Next_Reg (Ctx.E, "pui");
                              begin
                                 Add_Global_Decl (Ctx.E, PSym, Link_P & PSym & " = global i64 = 0, align 8");
                                 Emit_Inst (Ctx.E, PA & " = addr " & PSym);
                                 Emit_Inst (Ctx.E, "store.i64 0, " & PA & ", align 8");
                              end;
                           end if;
                           end;
                        end if;
                     end;
                  end if;
               end;

            when DN_GENERIC_DECL =>
               if Ctx.Enable_Checks and then not Ctx.In_Subunit then
                  declare
                     GID      : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                     GK       : constant Unsigned_32 := Tree_Key (GID);
                     Is_Exp_G : constant Boolean :=
                       Is_Global and then Export_All and then Is_Library_Level (GID);
                     Link_G   : constant String := (if Is_Exp_G then "export " else "");
                     GSym     : constant String :=
                       (if Is_Exp_G then "@ada_gelab__" & Qualified_Ada_Name (GID)
                        else "@ada_gelab_" & Tree_Key_Str (GID) & To_String (Ctx.Gen_Inst_Tag));
                     GA       : constant String := Next_Reg (Ctx.E, "gea");
                     GTI      : Type_Info;
                  begin
                     Add_Global_Decl (Ctx.E, GSym, Link_G & GSym & " = global i8 = 0, align 1");
                     Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                     Emit_Inst (Ctx.E, "store.i8 0, " & GA & ", align 1");
                     GTI.Pir_Type := To_Unbounded_String (GSym);
                     Ctx.Gen_Elab_Flags.Include (GK, GTI);
                  end;
               end if;

            when DN_SUBPROG_ENTRY_DECL =>
               declare
                  SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                  UK  : constant TREE := D (AS_UNIT_KIND, Item);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     Emit_Generic_Instantiation (Ctx, Item, Export_All);
                  elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                     -- Check if this procedure renames a task entry (LRM 8.5(9): evaluate prefix & family index ONCE!)
                     declare
                        RN      : TREE := D (AS_NAME, UK);
                        Fam_Exp : TREE := CONST_VOID;
                        Task_E  : TREE := CONST_VOID;
                        E_Id    : TREE := CONST_VOID;
                     begin
                        if not Is_Void_Tree (RN) and then KIND (RN) = DN_INDEXED then
                           declare
                              EL : SEQ_TYPE := LIST (D (AS_EXP_S, RN));
                           begin
                              if not IS_EMPTY (EL) then POP (EL, Fam_Exp); end if;
                           end;
                           RN := D (AS_NAME, RN);
                        end if;
                        if not Is_Void_Tree (RN) and then KIND (RN) = DN_SELECTED then
                           Task_E := D (AS_NAME, RN);
                           E_Id   := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, RN))));
                        elsif not Is_Void_Tree (RN) then
                           E_Id   := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (RN)));
                        end if;
                        if not Is_Void_Tree (E_Id) and then Ctx.Renamed_Entries.Contains (Tree_Key (E_Id)) then
                           declare
                              Src_RE : constant Renamed_Entry_Info := Ctx.Renamed_Entries.Element (Tree_Key (E_Id));
                              SK     : constant Unsigned_32 := Tree_Key (SID);
                              Cap_Sl : Natural := 0;
                           begin
                              if not Is_Global and then Ctx.Vars.Contains (SK)
                                and then Ctx.Vars.Element (SK).Owner_Subp = Ctx.Cur_Subp_Key
                              then
                                 Cap_Sl := Ctx.Vars.Element (SK).Captured_Slot;
                              end if;
                              if Cap_Sl > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                                 declare
                                    FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                    FP  : constant String := Next_Reg (Ctx.E, "refp");
                                 begin
                                    Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (Cap_Sl));
                                    if Src_RE.Is_Local and then Src_RE.Owner_Subp = Ctx.Cur_Subp_Key then
                                       Emit_Inst (Ctx.E, "store.ptr " & To_String (Src_RE.Task_Sym) & ", " & FP & ", align 8");
                                    else
                                       declare
                                          Pair_A : constant String := Next_Reg (Ctx.E, "renp");
                                          Off8   : constant String := Next_Reg (Ctx.E, "ren8");
                                          Off_I  : constant String := Next_Reg (Ctx.E, "reni");
                                          TS_G   : constant String :=
                                            (if Length (Src_RE.Fallback_TSym) > 0 then To_String (Src_RE.Fallback_TSym)
                                             else To_String (Src_RE.Task_Sym));
                                          IS_G   : constant String :=
                                            (if Length (Src_RE.Fallback_ISym) > 0 then To_String (Src_RE.Fallback_ISym)
                                             else To_String (Src_RE.Idx_Sym));
                                          GTA    : constant String := Next_Reg (Ctx.E, "rta");
                                          GIA    : constant String := Next_Reg (Ctx.E, "ria");
                                          GTV    : constant String := Next_Reg (Ctx.E, "rtv");
                                          GIV    : constant String := Next_Reg (Ctx.E, "riv");
                                       begin
                                          Emit_Entry_Alloca (Ctx.E, Pair_A & " = alloca [2 x i64], align 8");
                                          Emit_Inst (Ctx.E, GTA & " = addr " & TS_G);
                                          Emit_Inst (Ctx.E, GTV & " = load.ptr " & GTA & ", align 8");
                                          Emit_Inst (Ctx.E, "store.ptr " & GTV & ", " & Pair_A & ", align 8");
                                          Emit_Inst (Ctx.E, Off8 & " = const.i64 8");
                                          Emit_Inst (Ctx.E, Off_I & " = ptr_add.i64 " & Pair_A & ", " & Off8);
                                          Emit_Inst (Ctx.E, GIA & " = addr " & IS_G);
                                          Emit_Inst (Ctx.E, GIV & " = load.i32 " & GIA & ", align 4");
                                          Emit_Inst (Ctx.E, "store.i32 " & GIV & ", " & Off_I & ", align 4");
                                          Emit_Inst (Ctx.E, "store.ptr " & Pair_A & ", " & FP & ", align 8");
                                       end;
                                    end if;
                                 end;
                              end if;
                              Ctx.Renamed_Entries.Include (SK, Src_RE);
                           end;
                        elsif not Is_Void_Tree (E_Id) and then KIND (E_Id) = DN_ENTRY_ID then
                           declare
                              Task_Ptr : Unbounded_String;
                              EIdx_Reg : Unbounded_String := To_Unbounded_String ("0");
                              Is_Exp_R : constant Boolean := Is_Global and then Export_All and then Is_Library_Level (SID);
                              RLink    : constant String := (if Is_Exp_R then "export " else "");
                              RQName   : constant String := Qualified_Ada_Name (SID);
                              TSym     : constant String :=
                                (if Is_Exp_R then "@ada_rent_t__" & RQName
                                 else "@ada_rent_t_" & Tree_Key_Str (SID) & To_String (Ctx.Gen_Inst_Tag));
                              ISym     : constant String :=
                                (if Is_Exp_R then "@ada_rent_i__" & RQName
                                 else "@ada_rent_i_" & Tree_Key_Str (SID) & To_String (Ctx.Gen_Inst_Tag));
                              TA       : constant String := Next_Reg (Ctx.E, "rta");
                              IA       : constant String := Next_Reg (Ctx.E, "ria");
                           begin
                              if not Is_Void_Tree (Task_E) then
                                 declare
                                    TV : constant Expr_Val := Emit_Expr (Ctx, Task_E);
                                    TT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, TV.Ada_Type));
                                 begin
                                    if Is_Access_Type (TT) then
                                       Emit_Null_Check (Ctx, To_String (TV.Reg));
                                       declare
                                          LP : constant String := Next_Reg (Ctx.E, "ratp");
                                       begin
                                          Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (TV.Reg) & ", align 8");
                                          Task_Ptr := To_Unbounded_String (LP);
                                       end;
                                       if KIND (TT) = DN_ACCESS then
                                          Register_Task_Spec (Ctx, D (SM_DESIG_TYPE, TT), Ctx.Cur_Subp_Key);
                                       end if;
                                    else
                                       Task_Ptr := TV.Reg;
                                       Register_Task_Spec (Ctx, TT, Ctx.Cur_Subp_Key);
                                    end if;
                                 end;
                              else
                                 declare
                                    TK_T : Unsigned_32 := 0;
                                 begin
                                    if Ctx.Entries.Contains (Tree_Key (E_Id)) then
                                       TK_T := Ctx.Entries.Element (Tree_Key (E_Id)).Task_Key;
                                    end if;
                                    Task_Ptr := To_Unbounded_String (Emit_Enclosing_Task_Self_By_Key (Ctx, TK_T));
                                 end;
                              end if;
                              if Ctx.Entries.Contains (Tree_Key (E_Id)) then
                                 declare
                                    EI : constant Entry_Info := Ctx.Entries.Element (Tree_Key (E_Id));
                                 begin
                                    EIdx_Reg := To_Unbounded_String (Emit_Entry_Index_Reg (Ctx, EI, Fam_Exp));
                                 end;
                              end if;
                              if Is_Global then
                                 Add_Global_Decl (Ctx.E, TSym, RLink & TSym & " = global ptr = zeroinit, align 8");
                                 Add_Global_Decl (Ctx.E, ISym, RLink & ISym & " = global i32 = 0, align 4");
                                 Emit_Inst (Ctx.E, TA & " = addr " & TSym);
                                 Emit_Inst (Ctx.E, "store.ptr " & To_String (Task_Ptr) & ", " & TA & ", align 8");
                                 Emit_Inst (Ctx.E, IA & " = addr " & ISym);
                                 Emit_Inst (Ctx.E, "store.i32 " & To_String (EIdx_Reg) & ", " & IA & ", align 4");
                                 Ctx.Renamed_Entries.Include (Tree_Key (SID),
                                   (Task_Sym      => To_Unbounded_String (TSym),
                                    Idx_Sym       => To_Unbounded_String (ISym),
                                    Entry_Id      => E_Id,
                                    Is_Local      => False,
                                    Owner_Subp    => 0,
                                    Fallback_TSym => Null_Unbounded_String,
                                    Fallback_ISym => Null_Unbounded_String));
                              else
                                 -- Local task-entry renaming: allocate on stack (with static-link capture if needed)
                                 -- so recursive and concurrent calls never clobber each other!
                                 -- Also mirror into private globals for non-captured cross-subp / generic formal defaults (cc3602a).
                                 declare
                                    Pair_A : constant String := Next_Reg (Ctx.E, "renp");
                                    Off8   : constant String := Next_Reg (Ctx.E, "ren8");
                                    Off_I  : constant String := Next_Reg (Ctx.E, "reni");
                                    SK     : constant Unsigned_32 := Tree_Key (SID);
                                    Cap_Sl : Natural := 0;
                                 begin
                                    Emit_Entry_Alloca (Ctx.E, Pair_A & " = alloca [2 x i64], align 8");
                                    Emit_Inst (Ctx.E, "store.ptr " & To_String (Task_Ptr) & ", " & Pair_A & ", align 8");
                                    Emit_Inst (Ctx.E, Off8 & " = const.i64 8");
                                    Emit_Inst (Ctx.E, Off_I & " = ptr_add.i64 " & Pair_A & ", " & Off8);
                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (EIdx_Reg) & ", " & Off_I & ", align 4");
                                    Add_Global_Decl (Ctx.E, TSym, TSym & " = global ptr = zeroinit, align 8");
                                    Add_Global_Decl (Ctx.E, ISym, ISym & " = global i32 = 0, align 4");
                                    Emit_Inst (Ctx.E, TA & " = addr " & TSym);
                                    Emit_Inst (Ctx.E, "store.ptr " & To_String (Task_Ptr) & ", " & TA & ", align 8");
                                    Emit_Inst (Ctx.E, IA & " = addr " & ISym);
                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (EIdx_Reg) & ", " & IA & ", align 4");
                                    if Ctx.Vars.Contains (SK) and then Ctx.Vars.Element (SK).Owner_Subp = Ctx.Cur_Subp_Key then
                                       Cap_Sl := Ctx.Vars.Element (SK).Captured_Slot;
                                    end if;
                                    if Cap_Sl > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                                       declare
                                          FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                          FP  : constant String := Next_Reg (Ctx.E, "refp");
                                       begin
                                          Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (Cap_Sl));
                                          Emit_Inst (Ctx.E, "store.ptr " & Pair_A & ", " & FP & ", align 8");
                                       end;
                                    end if;
                                    Ctx.Renamed_Entries.Include (SK,
                                      (Task_Sym      => To_Unbounded_String (Pair_A),
                                       Idx_Sym       => To_Unbounded_String (Off_I),
                                       Entry_Id      => E_Id,
                                       Is_Local      => True,
                                       Owner_Subp    => Ctx.Cur_Subp_Key,
                                       Fallback_TSym => To_Unbounded_String (TSym),
                                       Fallback_ISym => To_Unbounded_String (ISym)));
                                 end;
                              end if;
                           end;
                        end if;
                     end;
                  elsif Is_Void_Tree (UK) and then Ctx.Enable_Checks and then not Ctx.In_Subunit
                    and then KIND (SID) /= DN_GENERIC_ID and then Ctx.Subps.Contains (Tree_Key (SID))
                  then
                     declare
                        SK   : constant Unsigned_32 := Tree_Key (SID);
                        SI   : Subp_Info := Ctx.Subps.Element (SK);
                        GSym : constant String := "@ada_selab_" & Tree_Key_Str (SID) & To_String (Ctx.Gen_Inst_Tag);
                        GA   : constant String := Next_Reg (Ctx.E, "sea");
                     begin
                        Add_Global_Decl (Ctx.E, GSym, GSym & " = global i8 = 0, align 1");
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        Emit_Inst (Ctx.E, "store.i8 0, " & GA & ", align 1");
                        SI.Elab_Flag := To_Unbounded_String (GSym);
                        Ctx.Subps.Replace (SK, SI);
                     end;
                  end if;
               end;

            when DN_PACKAGE_DECL =>
               declare
                  PID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                  UK  : constant TREE := D (AS_UNIT_KIND, Item);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     declare
                        Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                     begin
                        if not Is_RTL_Generic (Gen_Id) or else not Is_Global then
                           Emit_Generic_Instantiation (Ctx, Item, Export_All);
                        end if;
                     end;
                  else
                  declare
                     Hdr : constant TREE := D (AS_HEADER, Item);
                     Has_Stub_Bdy : Boolean := False;
                     Has_Any_Bdy  : Boolean := False;
                  begin
                     -- Check if this local package has a body (or SEPARATE body stub) in this declarative part
                     if not Is_Void_Tree (Item_S) then
                     declare
                        SL : SEQ_TYPE := LIST (Item_S);
                        SI_N : TREE;
                     begin
                        while not IS_EMPTY (SL) loop
                           POP (SL, SI_N);
                           if KIND (SI_N) = DN_PACKAGE_BODY
                             and then Canonical_Def (D (AS_SOURCE_NAME, SI_N)) = PID
                           then
                              Has_Any_Bdy := True;
                              if not Is_Void_Tree (D (AS_BODY, SI_N))
                                and then KIND (D (AS_BODY, SI_N)) = DN_STUB
                              then
                                 Has_Stub_Bdy := True;
                              end if;
                              exit;
                           end if;
                        end loop;
                     end;
                     end if;
                     if Is_Void_Tree (UK) and then not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                        declare
                           -- P3-4.9: A nested package declared inside an enclosing package spec that requires a body
                           -- (e.g. declares a task/subprogram/generic) must get its own Pkg_Gid so its tasks activate
                           -- before its own body statements (LRM 9.3(2)), not when the outer package begins!
                           function Spec_List_Requires_Body (DS : TREE) return Boolean is
                              SL2 : SEQ_TYPE;
                              I2  : TREE;
                           begin
                              if Is_Void_Tree (DS) then return False; end if;
                              SL2 := LIST (DS);
                              while not IS_EMPTY (SL2) loop
                                 POP (SL2, I2);
                                 case KIND (I2) is
                                    when DN_TASK_DECL =>
                                       return True;
                                    when DN_SUBPROG_ENTRY_DECL =>
                                       if Is_Void_Tree (D (AS_UNIT_KIND, I2)) then
                                          return True;
                                       end if;
                                    when DN_GENERIC_DECL =>
                                       return True;
                                    when DN_PACKAGE_DECL =>
                                       if Is_Void_Tree (D (AS_UNIT_KIND, I2))
                                         and then not Is_Void_Tree (D (AS_HEADER, I2))
                                         and then KIND (D (AS_HEADER, I2)) = DN_PACKAGE_SPEC
                                       then
                                          if Spec_List_Requires_Body (D (AS_DECL_S1, D (AS_HEADER, I2)))
                                            or else Spec_List_Requires_Body (D (AS_DECL_S2, D (AS_HEADER, I2)))
                                          then
                                             return True;
                                          end if;
                                       end if;
                                    when others =>
                                       null;
                                 end case;
                              end loop;
                              return False;
                           end Spec_List_Requires_Body;
                           Nested_Needs_Bdy : constant Boolean :=
                             not Has_Any_Bdy
                             and then (Spec_List_Requires_Body (D (AS_DECL_S1, Hdr))
                                       or else Spec_List_Requires_Body (D (AS_DECL_S2, Hdr)));
                           Sub_Dyn : Boolean;
                           Saved_G : constant Integer_64 := Ctx.Cur_Task_Group_Id;
                           Gid     : Integer_64 := 0;
                           Pkg_G   : constant Boolean := Is_Global or else Has_Stub_Bdy;
                           Exp_1   : constant Boolean := (if Is_Global then Export_All else Has_Stub_Bdy);
                           Exp_2   : constant Boolean := Exp_1;
                        begin
                           -- LRM 9.3(2): only a package WITH a body activates its tasks at the package body's begin;
                           -- a bodyless package defers task activation to the enclosing declarative part's begin!
                           if Has_Any_Bdy or else Nested_Needs_Bdy then
                              if Has_Stub_Bdy or else (Nested_Needs_Bdy and then Is_Global) then
                                 Gid := Integer_64 (Lib_Binder.Lib_Pkg_Task_Group_Id
                                   (Ada.Strings.Fixed.Translate (Qualified_Ada_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map)));
                                 Ctx.Pkg_Task_Groups.Include (Tree_Key (PID), Gid);
                              else
                                 Gid := Ctx.Next_Task_Group_Id;
                                 Ctx.Next_Task_Group_Id := Ctx.Next_Task_Group_Id + 1;
                                 Ctx.Pkg_Task_Groups.Include (Tree_Key (PID), Gid);
                              end if;
                              Ctx.Cur_Task_Group_Id := Gid;
                           end if;
                           Emit_Decl_Items (Ctx, D (AS_DECL_S1, Hdr), Is_Global => Pkg_G, Export_All => Exp_1, Has_Dyn => Sub_Dyn);
                           Emit_Decl_Items (Ctx, D (AS_DECL_S2, Hdr), Is_Global => Pkg_G, Export_All => Exp_2, Has_Dyn => Sub_Dyn);
                           if Gid /= 0 then
                              Ctx.Cur_Task_Group_Id := Saved_G;
                           end if;
                           if Sub_Dyn then Has_Dyn := True; end if;
                        end;
                     end if;
                  end;
                  end if;
               end;

            when DN_PACKAGE_BODY =>
               declare
                  Orig_PID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                  PID : constant TREE := Canonical_Def (Subst_Tree (Ctx, Orig_PID));
                  Bdy : TREE := D (AS_BODY, Item);
               begin
                  -- Inline separate nested package subunit inside a generic body (LRM 10.2/12.3)
                  if KIND (PID) /= DN_GENERIC_ID and then not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_STUB then
                     if Ctx.Gen_Subunits.Contains (Tree_Key (Orig_PID)) then
                        Bdy := D (AS_BODY, Ctx.Gen_Subunits.Element (Tree_Key (Orig_PID)));
                     elsif Ctx.Gen_Subunits.Contains (Tree_Key (PID)) then
                        Bdy := D (AS_BODY, Ctx.Gen_Subunits.Element (Tree_Key (PID)));
                     end if;
                  end if;
                  if KIND (PID) = DN_GENERIC_ID then
                     if Ctx.Gen_Elab_Flags.Contains (Tree_Key (PID)) then
                        declare
                           GSym : constant String := To_String (Ctx.Gen_Elab_Flags.Element (Tree_Key (PID)).Pir_Type);
                           GA   : constant String := Next_Reg (Ctx.E, "ge1");
                        begin
                           Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                           Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                        end;
                     elsif Ctx.Enable_Checks and then not Ctx.In_Subunit and then Is_Library_Level (PID) then
                        declare
                           GSym : constant String := "@ada_gelab__" & Qualified_Ada_Name (PID);
                           GA   : constant String := Next_Reg (Ctx.E, "ge1");
                        begin
                           Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global i8, align 1");
                           Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                           Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                        end;
                     end if;
                  elsif not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_BLOCK_BODY then
                     declare
                        Sub_Dyn : Boolean;
                        Alt_S   : constant TREE := D (AS_ALTERNATIVE_S, Bdy);
                        Has_Hdl : constant Boolean := not Is_Void_Tree (Alt_S) and then not IS_EMPTY (LIST (Alt_S));
                        Hdl_Lbl, After_Lbl : Unbounded_String;
                        Bdy_Has_T : constant Boolean := Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Bdy));
                        Saved_G   : constant Integer_64 := Ctx.Cur_Task_Group_Id;
                        Gid       : Integer_64 := 0;
                        Dummy     : Unbounded_String;
                     begin
                        if Ctx.Pkg_Task_Groups.Contains (Tree_Key (PID)) then
                           Gid := Ctx.Pkg_Task_Groups.Element (Tree_Key (PID));
                           Ctx.Pkg_Task_Groups.Delete (Tree_Key (PID));
                        elsif Is_Global and then KIND (PID) = DN_PACKAGE_ID
                          and then not Is_Void_Tree (D (SM_SPEC, PID))
                          and then KIND (D (SM_SPEC, PID)) = DN_PACKAGE_SPEC
                          and then (Item_List_Has_Tasks (Ctx, D (AS_DECL_S1, D (SM_SPEC, PID)))
                                    or else Item_List_Has_Tasks (Ctx, D (AS_DECL_S2, D (SM_SPEC, PID)))
                                    or else Bdy_Has_T)
                        then
                           -- P3-4.9: Nested package inside separately-compiled enclosing library package spec
                           Gid := Integer_64 (Lib_Binder.Lib_Pkg_Task_Group_Id
                             (Ada.Strings.Fixed.Translate (Qualified_Ada_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map)));
                        elsif Bdy_Has_T then
                           Gid := Ctx.Next_Task_Group_Id;
                           Ctx.Next_Task_Group_Id := Ctx.Next_Task_Group_Id + 1;
                        end if;
                        if Gid /= 0 then
                           Ctx.Cur_Task_Group_Id := Gid;
                        end if;
                        Emit_Decl_Items (Ctx, D (AS_ITEM_S, Bdy), Is_Global => Is_Global, Export_All => Export_All, Has_Dyn => Sub_Dyn);
                        if Gid /= 0 then
                           Ctx.Cur_Task_Group_Id := Saved_G;
                        end if;
                        if Has_Hdl then
                           Hdl_Lbl   := To_Unbounded_String (Next_Label (Ctx.E, "pexc_h"));
                           After_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "pexc_end"));
                           Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                                 Current_Exn   => Null_Unbounded_String,
                                                 In_Handler    => False,
                                                 SS_Mark       => Null_Unbounded_String,
                                                 SP_Mark       => Null_Unbounded_String));
                        end if;
                        if Gid /= 0 then
                           -- LRM 9.3(2): activate ONLY tasks belonging to this package's group_id!
                           declare
                              Mst_R : Unbounded_String := Ctx.Cur_Master_Reg;
                           begin
                              if Length (Mst_R) = 0 then
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", Mst_R, Nounwind => True);
                              end if;
                              Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_group",
                                To_String (Mst_R) & ", " & Img (Long_Long_Integer (Gid)), "", Dummy);
                           end;
                        end if;
                        Scan_Label_Master_Depths (Ctx, D (AS_STM_S, Bdy),
                          Natural (Ctx.Master_Stack.Length), Natural (Ctx.SS_Marks.Length), Natural (Ctx.SP_Marks.Length));
                        Scan_Label_Master_Depths (Ctx, Alt_S,
                          Natural (Ctx.Master_Stack.Length), Natural (Ctx.SS_Marks.Length), Natural (Ctx.SP_Marks.Length));
                        Emit_Stm_List (Ctx, D (AS_STM_S, Bdy));
                        if Has_Hdl then
                           if not Is_Terminated (Ctx.E) then
                              Emit_Term (Ctx.E, "jmp " & To_String (After_Lbl));
                           end if;
                           Emit_Exception_Handlers (Ctx, Alt_S, To_String (Hdl_Lbl), To_String (After_Lbl));
                           Ctx.Handlers.Delete_Last;
                           Emit_Label (Ctx.E, To_String (After_Lbl));
                        end if;
                        if Sub_Dyn then Has_Dyn := True; end if;
                     end;
                  elsif KIND (PID) /= DN_GENERIC_ID and then not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_STUB then
                     -- Local or library package body stub (PACKAGE BODY P IS SEPARATE;):
                     -- Re-register spec subprograms with Is_Subunit => True + extern, and call @ada__<pname>__elab_body!
                     declare
                        PSpec : constant TREE := D (SM_SPEC, PID);
                        PName : constant String := Qualified_Ada_Name (PID);
                        Elab_S : constant String := "@ada__" & PName & "__elab_body";
                        Dummy : Unbounded_String;

                        procedure Mark_Subunit_Subps (DS : TREE) is
                           PL : SEQ_TYPE;
                           PI : TREE;
                        begin
                           if Is_Void_Tree (DS) then return; end if;
                           PL := LIST (DS);
                           while not IS_EMPTY (PL) loop
                              POP (PL, PI);
                              if KIND (PI) = DN_SUBPROG_ENTRY_DECL then
                                 declare
                                    SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, PI));
                                    PUK : constant TREE := D (AS_UNIT_KIND, PI);
                                 begin
                                    if Is_Void_Tree (PUK) and then KIND (SID) /= DN_GENERIC_ID then
                                       declare
                                          K_S : constant Unsigned_32 := Tree_Key (SID);
                                          SI  : Subp_Info;
                                       begin
                                          if Ctx.Subps.Contains (K_S) then
                                             SI := Ctx.Subps.Element (K_S);
                                          end if;
                                          SI.Spec_Node   := D (AS_HEADER, PI);
                                          SI.Pir_Name    := To_Unbounded_String (Mangle_Subp_Name (Ctx, SID, SI.Spec_Node, Is_Subunit => True));
                                          SI.Is_External := True;
                                          SI.Is_Function := KIND (SI.Spec_Node) = DN_FUNCTION_SPEC;
                                          if SI.Is_Function then
                                             declare
                                                RDef : constant TREE := Get_Name_Defn (D (AS_NAME, SI.Spec_Node));
                                             begin
                                                SI.Ret_Type := (if not Is_Void_Tree (RDef) then Subst_Tree (Ctx, D (SM_TYPE_SPEC, RDef)) else CONST_VOID);
                                             end;
                                             declare
                                                RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                                             begin
                                                SI.Ret_Composite := RTI.Kind /= TK_Scalar;
                                                SI.Ret_Uncon_Arr := RTI.Kind = TK_Unconstrained_Array
                                                  or else (RTI.Kind = TK_Constrained_Array and then RTI.Is_Dynamic);
                                             end;
                                          end if;
                                          Ctx.Subps.Include (K_S, SI);
                                          Ensure_Extern_Subp (Ctx, SID, SI);
                                       end;
                                    end if;
                                 end;
                              end if;
                           end loop;
                        end Mark_Subunit_Subps;
                     begin
                        if not Is_Void_Tree (PSpec) and then KIND (PSpec) = DN_PACKAGE_SPEC then
                           Mark_Subunit_Subps (D (AS_DECL_S1, PSpec));
                           Mark_Subunit_Subps (D (AS_DECL_S2, PSpec));
                        end if;
                        Add_Extern_Func (Ctx.E, Elab_S, "extern func " & Elab_S & "() -> void cc ""internal""");
                        -- P3-4.9: Propagate enclosing local master to separate package body subunit via TLS active master
                        if Length (Ctx.Cur_Master_Reg) > 0 then
                           declare
                              Prev_AM : Unbounded_String;
                              L_Unw   : constant String := Next_Label (Ctx.E, "spbu");
                              L_Ok    : constant String := Next_Label (Ctx.E, "spbo");
                              Ex_R    : constant String := Next_Reg (Ctx.E, "spbe");
                              Dummy2  : Unbounded_String;
                           begin
                              Emit_Call_Or_Invoke (Ctx, "@__ada_set_active_master", To_String (Ctx.Cur_Master_Reg), "ptr", Prev_AM, Nounwind => True);
                              Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (L_Unw),
                                                    Current_Exn   => Null_Unbounded_String,
                                                    In_Handler    => False,
                                                    SS_Mark       => Null_Unbounded_String,
                                                    SP_Mark       => Null_Unbounded_String));
                              Emit_Call_Or_Invoke (Ctx, Elab_S, "", "", Dummy);
                              Ctx.Handlers.Delete_Last;
                              Emit_Call_Or_Invoke (Ctx, "@__ada_set_active_master", To_String (Prev_AM), "ptr", Dummy2, Nounwind => True);
                              Emit_Term (Ctx.E, "jmp " & L_Ok);
                              Emit_Label (Ctx.E, L_Unw, Ex_R & ": ptr");
                              Emit_Call_Or_Invoke (Ctx, "@__ada_set_active_master", To_String (Prev_AM), "ptr", Dummy2, Nounwind => True);
                              Emit_Raise_Reg (Ctx, Ex_R);
                              Emit_Label (Ctx.E, L_Ok);
                           end;
                        else
                           Emit_Call_Or_Invoke (Ctx, Elab_S, "", "", Dummy);
                        end if;
                     end;
                  end if;
               end;

            when DN_TASK_DECL =>
               -- Check entry family discrete ranges during task declaration elaboration (LRM 9.5/9.3)
               declare
                  Src : constant TREE := D (AS_SOURCE_NAME, Item);
                  TS_Check : TREE := CONST_VOID;
               begin
                  if not Is_Void_Tree (Src) then
                     if KIND (Src) in CLASS_TYPE_NAME then TS_Check := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Src)));
                     elsif KIND (Src) = DN_VARIABLE_ID then TS_Check := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src)));
                     end if;
                  end if;
                  -- LRM 3.9: track task body elaboration flag (c39008a/b/c, c93007a); P3-4.3: stack-local for local tasks!
                  if Ctx.Enable_Checks and then not Ctx.In_Subunit
                    and then not Is_Void_Tree (TS_Check) and then KIND (TS_Check) = DN_TASK_SPEC
                    and then Ctx.Tasks.Contains (Tree_Key (TS_Check))
                  then
                     declare
                        TK       : constant Unsigned_32 := Tree_Key (TS_Check);
                        TI       : Task_Info := Ctx.Tasks.Element (TK);
                        Is_Exp_T : constant Boolean :=
                          Is_Global and then Export_All and then Is_Library_Level (Src);
                        Link_T   : constant String := (if Is_Exp_T then "export " else "");
                        GSym     : constant String :=
                          (if Is_Exp_T then "@ada_telab__" & Qualified_Ada_Name (Src)
                           else "@ada_telab_" & Tree_Key_Str (TS_Check) & To_String (Ctx.Gen_Inst_Tag));
                        GA       : constant String := Next_Reg (Ctx.E, "tea");
                     begin
                        if not Is_Global and then Ctx.Cur_Subp_Key /= 0 then
                           declare
                              TLoc   : constant String := Next_Reg (Ctx.E, "tel");
                              Cap_Sl : Natural := 0;
                           begin
                              Emit_Entry_Alloca (Ctx.E, TLoc & " = alloca i8, align 1");
                              Emit_Inst (Ctx.E, "store.i8 0, " & TLoc & ", align 1");
                              if Ctx.Vars.Contains (TK) and then Ctx.Vars.Element (TK).Owner_Subp = Ctx.Cur_Subp_Key then
                                 Cap_Sl := Ctx.Vars.Element (TK).Captured_Slot;
                              end if;
                              if Cap_Sl > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                                 declare
                                    FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                    FP  : constant String := Next_Reg (Ctx.E, "tefp");
                                 begin
                                    Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (Cap_Sl));
                                    Emit_Inst (Ctx.E, "store.ptr " & TLoc & ", " & FP & ", align 8");
                                 end;
                              end if;
                              Add_Global_Decl (Ctx.E, GSym, GSym & " = global i8 = 0, align 1");
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store.i8 0, " & GA & ", align 1");
                              TI.Elab_Flag     := To_Unbounded_String (TLoc);
                              TI.Elab_Is_Local := True;
                              TI.Elab_Owner    := Ctx.Cur_Subp_Key;
                              TI.Elab_Cap_Slot := Cap_Sl;
                              TI.Elab_Fallback := To_Unbounded_String (GSym);
                              Ctx.Tasks.Replace (TK, TI);
                           end;
                        else
                           Add_Global_Decl (Ctx.E, GSym, Link_T & GSym & " = global i8 = 0, align 1");
                           Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                           Emit_Inst (Ctx.E, "store.i8 0, " & GA & ", align 1");
                           TI.Elab_Flag     := To_Unbounded_String (GSym);
                           TI.Elab_Is_Local := False;
                           Ctx.Tasks.Replace (TK, TI);
                        end if;
                     end;
                  end if;
                  -- P3-4.10: Elaborate & save dynamic entry family bounds/base/count ONCE at DN_TASK_DECL
                  if not Is_Void_Tree (TS_Check) and then KIND (TS_Check) = DN_TASK_SPEC
                    and then not Is_Void_Tree (D (SM_DECL_S, TS_Check))
                  then
                     declare
                        TK_E       : constant Unsigned_32 := Tree_Key (TS_Check);
                        Has_Dyn_E  : constant Boolean :=
                          Ctx.Tasks.Contains (TK_E) and then Ctx.Tasks.Element (TK_E).Has_Dyn_Entries;
                        Is_Exp_EF  : constant Boolean :=
                          Is_Global and then Export_All and then Is_Library_Level (Src);
                        EF_Link    : constant String := (if Is_Exp_EF then "export " else "");
                        Cur_Base   : Unbounded_String := To_Unbounded_String ("0");
                        EL         : SEQ_TYPE := LIST (D (SM_DECL_S, TS_Check));
                        EN         : TREE;
                     begin
                        if Has_Dyn_E then
                           declare
                              Z0 : constant String := Next_Reg (Ctx.E, "efb0");
                           begin
                              Emit_Inst (Ctx.E, Z0 & " = const.i32 0");
                              Cur_Base := To_Unbounded_String (Z0);
                           end;
                        end if;
                        while not IS_EMPTY (EL) loop
                           POP (EL, EN);
                           if KIND (EN) = DN_SUBPROG_ENTRY_DECL then
                              declare
                                 EID  : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, EN));
                                 EK_E : constant Unsigned_32 := Tree_Key (EID);
                                 Hdr  : constant TREE := D (AS_HEADER, EN);
                                 DR   : constant TREE := (if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_ENTRY then D (AS_DISCRETE_RANGE, Hdr) else CONST_VOID);
                                 Has_EI_E : constant Boolean := Ctx.Entries.Contains (EK_E);
                                 EI_E     : Entry_Info;
                                 Lo32_E, Hi32_E : Unbounded_String;
                                 Have_Dyn_Bounds : Boolean := False;
                              begin
                                 if Has_EI_E then
                                    EI_E := Ctx.Entries.Element (EK_E);
                                 end if;
                                 if Has_Dyn_E and then Has_EI_E and then Length (EI_E.Fam_Base_Sym) > 0 then
                                    declare
                                       BSym : constant String := To_String (EI_E.Fam_Base_Sym);
                                       BA   : constant String := Next_Reg (Ctx.E, "efbd");
                                    begin
                                       Add_Global_Decl (Ctx.E, BSym, EF_Link & BSym & " = global i32 = 0, align 4");
                                       Emit_Inst (Ctx.E, BA & " = addr " & BSym);
                                       Emit_Inst (Ctx.E, "store.i32 " & To_String (Cur_Base) & ", " & BA & ", align 4");
                                    end;
                                 end if;
                                 if not Is_Void_Tree (DR) and then KIND (DR) = DN_DISCRETE_SUBTYPE then
                                    declare
                                       SI_N : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                                       C_N  : constant TREE := (if KIND (SI_N) = DN_SUBTYPE_INDICATION then D (AS_CONSTRAINT, SI_N) else CONST_VOID);
                                    begin
                                       if not Is_Void_Tree (C_N) and then KIND (C_N) in DN_RANGE | DN_RANGE_ATTRIBUTE then
                                          declare
                                             Base_T : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, SI_N))));
                                             PT_U, Lo_R, Hi_R : Unbounded_String;
                                             Uns_B    : Boolean;
                                          begin
                                             Emit_Discrete_Range (Ctx, C_N, PT_U, Lo_R, Hi_R);
                                             Uns_B := Is_Unsigned_Scalar (Base_T, To_String (PT_U));
                                             if Ctx.Enable_Checks then
                                                declare
                                                   Non_Null : constant String := Next_Reg (Ctx.E, "efnn");
                                                   L_Chk    : constant String := Next_Label (Ctx.E, "efc");
                                                   L_Done   : constant String := Next_Label (Ctx.E, "efd");
                                                   Le_Op    : constant String := (if Uns_B then "cmp_le_u." else "cmp_le_s.");
                                                begin
                                                   Emit_Inst (Ctx.E, Non_Null & " = " & Le_Op & To_String (PT_U) & " " & To_String (Lo_R) & ", " & To_String (Hi_R));
                                                   Emit_Term (Ctx.E, "br " & Non_Null & ", " & L_Chk & ", " & L_Done);
                                                   Emit_Label (Ctx.E, L_Chk);
                                                   Emit_Range_Check (Ctx, To_String (Lo_R), To_String (PT_U), Base_T);
                                                   Emit_Range_Check (Ctx, To_String (Hi_R), To_String (PT_U), Base_T);
                                                   Emit_Term (Ctx.E, "jmp " & L_Done);
                                                   Emit_Label (Ctx.E, L_Done);
                                                end;
                                             end if;
                                             Lo32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (PT_U), "i32", Is_Unsigned => Uns_B));
                                             Hi32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (PT_U), "i32", Is_Unsigned => Uns_B));
                                             Have_Dyn_Bounds := True;
                                          end;
                                       elsif Has_Dyn_E and then Has_EI_E and then not EI_E.Fam_Is_Static then
                                          declare
                                             PT_U, Lo_R, Hi_R : Unbounded_String;
                                          begin
                                             Emit_Discrete_Range (Ctx, DR, PT_U, Lo_R, Hi_R);
                                             Lo32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (PT_U), "i32"));
                                             Hi32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (PT_U), "i32"));
                                             Have_Dyn_Bounds := True;
                                          end;
                                       end if;
                                    end;
                                 elsif not Is_Void_Tree (DR) and then KIND (DR) in DN_RANGE | DN_RANGE_ATTRIBUTE then
                                    declare
                                       PT_U, Lo_R, Hi_R : Unbounded_String;
                                    begin
                                       if Ctx.Enable_Checks or else (Has_Dyn_E and then Has_EI_E and then not EI_E.Fam_Is_Static) then
                                          Emit_Discrete_Range (Ctx, DR, PT_U, Lo_R, Hi_R);
                                          Lo32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (PT_U), "i32"));
                                          Hi32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (PT_U), "i32"));
                                          Have_Dyn_Bounds := True;
                                       end if;
                                    end;
                                 end if;
                                 if Has_Dyn_E and then Has_EI_E then
                                    if EI_E.Is_Family and then not EI_E.Fam_Is_Static and then Have_Dyn_Bounds then
                                       declare
                                          LSym : constant String := To_String (EI_E.Fam_Lo_Sym);
                                          HSym : constant String := To_String (EI_E.Fam_Hi_Sym);
                                          LA   : constant String := Next_Reg (Ctx.E, "efld");
                                          HA   : constant String := Next_Reg (Ctx.E, "efhd");
                                          Ge   : constant String := Next_Reg (Ctx.E, "efge");
                                          Df   : constant String := Next_Reg (Ctx.E, "efdf");
                                          P1   : constant String := Next_Reg (Ctx.E, "efp1");
                                          Cnt  : constant String := Next_Reg (Ctx.E, "efct");
                                          Nxt  : constant String := Next_Reg (Ctx.E, "efnx");
                                       begin
                                          Add_Global_Decl (Ctx.E, LSym, EF_Link & LSym & " = global i32 = 0, align 4");
                                          Add_Global_Decl (Ctx.E, HSym, EF_Link & HSym & " = global i32 = -1, align 4");
                                          Emit_Inst (Ctx.E, LA & " = addr " & LSym);
                                          Emit_Inst (Ctx.E, "store.i32 " & To_String (Lo32_E) & ", " & LA & ", align 4");
                                          Emit_Inst (Ctx.E, HA & " = addr " & HSym);
                                          Emit_Inst (Ctx.E, "store.i32 " & To_String (Hi32_E) & ", " & HA & ", align 4");
                                          Emit_Inst (Ctx.E, Ge & " = cmp_ge_s.i32 " & To_String (Hi32_E) & ", " & To_String (Lo32_E));
                                          Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (Hi32_E) & ", " & To_String (Lo32_E));
                                          Emit_Inst (Ctx.E, P1 & " = add.i32 " & Df & ", 1");
                                          Emit_Inst (Ctx.E, Cnt & " = select.i32 " & Ge & ", " & P1 & ", 0");
                                          Emit_Inst (Ctx.E, Nxt & " = add.i32 " & To_String (Cur_Base) & ", " & Cnt);
                                          Cur_Base := To_Unbounded_String (Nxt);
                                       end;
                                    elsif EI_E.Is_Family and then EI_E.Fam_Is_Static then
                                       declare
                                          Sz_S : constant Long_Long_Integer :=
                                            (if EI_E.Fam_Hi >= EI_E.Fam_Lo then Long_Long_Integer (EI_E.Fam_Hi - EI_E.Fam_Lo + 1) else 0);
                                          Nxt  : constant String := Next_Reg (Ctx.E, "efnx");
                                       begin
                                          if Sz_S > 0 then
                                             Emit_Inst (Ctx.E, Nxt & " = add.i32 " & To_String (Cur_Base) & ", " & Img (Sz_S));
                                             Cur_Base := To_Unbounded_String (Nxt);
                                          end if;
                                       end;
                                    else
                                       declare
                                          Nxt : constant String := Next_Reg (Ctx.E, "efnx");
                                       begin
                                          Emit_Inst (Ctx.E, Nxt & " = add.i32 " & To_String (Cur_Base) & ", 1");
                                          Cur_Base := To_Unbounded_String (Nxt);
                                       end;
                                    end if;
                                 end if;
                              end;
                           end if;
                        end loop;
                        if Has_Dyn_E and then Ctx.Tasks.Contains (TK_E) then
                           declare
                              TI_E : constant Task_Info := Ctx.Tasks.Element (TK_E);
                              NSym : constant String := To_String (TI_E.Dyn_Entries_Sym);
                              Gt0  : constant String := Next_Reg (Ctx.E, "efg0");
                              Tot  : constant String := Next_Reg (Ctx.E, "eftt");
                              NA   : constant String := Next_Reg (Ctx.E, "efna");
                           begin
                              if NSym'Length > 0 then
                                 Add_Global_Decl (Ctx.E, NSym, EF_Link & NSym & " = global i32 = 1, align 4");
                                 Emit_Inst (Ctx.E, Gt0 & " = cmp_gt_s.i32 " & To_String (Cur_Base) & ", 0");
                                 Emit_Inst (Ctx.E, Tot & " = select.i32 " & Gt0 & ", " & To_String (Cur_Base) & ", 1");
                                 Emit_Inst (Ctx.E, NA & " = addr " & NSym);
                                 Emit_Inst (Ctx.E, "store.i32 " & Tot & ", " & NA & ", align 4");
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;
               -- Single anonymous task object (`task T is ... end T;`): allocate and create task handle
               declare
                  Src : constant TREE := D (AS_SOURCE_NAME, Item);
               begin
                  if not Is_Void_Tree (Src) and then KIND (Src) = DN_VARIABLE_ID then
                     declare
                        Can_Id : constant TREE := Canonical_Def (Src);
                        Key    : constant Unsigned_32 := Tree_Key (Can_Id);
                        TS     : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src));
                        VInfo  : Var_Info;
                        TH     : constant String := Emit_Create_Task_Handle (Ctx, TS, Activate_Now => False);
                     begin
                        if Is_Global then
                           declare
                              GSym : constant String :=
                                (if Export_All and then Is_Library_Level (Can_Id) then "@ada_g__" & Qualified_Ada_Name (Can_Id)
                                 else "@ada_g__" & Qualified_Ada_Name (Can_Id) & "_" & Tree_Key_Str (Can_Id) & To_String (Ctx.Gen_Inst_Tag));
                              Link : constant String := (if Export_All and then Is_Library_Level (Can_Id) then "export " else "");
                              GA   : constant String := Next_Reg (Ctx.E, "tga");
                           begin
                              Add_Global_Decl (Ctx.E, GSym, Link & GSym & " = global ptr = zeroinit, align 8");
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store.ptr " & TH & ", " & GA & ", align 8");
                              VInfo.Kind       := VK_Global;
                              VInfo.Pir_Name   := To_Unbounded_String (GSym);
                              VInfo.Pir_Type   := To_Unbounded_String ("ptr");
                              VInfo.Ada_Type   := TS;
                              VInfo.Align      := 8;
                              VInfo.Owner_Subp := 0;
                              Ctx.Vars.Include (Key, VInfo);
                           end;
                        else
                           declare
                              Reg : constant String := Next_Reg (Ctx.E, "t_" & Sanitize_Ident (Sym_Name (Src)) & "_");
                           begin
                              Emit_Entry_Alloca (Ctx.E, Reg & " = alloca ptr, align 8");
                              Emit_Inst (Ctx.E, "store.ptr " & TH & ", " & Reg & ", align 8");
                              VInfo.Kind       := VK_Local_Alloca;
                              VInfo.Pir_Name   := To_Unbounded_String (Reg);
                              VInfo.Pir_Type   := To_Unbounded_String ("ptr");
                              VInfo.Ada_Type   := TS;
                              VInfo.Align      := 8;
                              VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                              if Ctx.Vars.Contains (Key) and then Ctx.Vars.Element (Key).Owner_Subp = Ctx.Cur_Subp_Key then
                                 VInfo.Captured_Slot := Ctx.Vars.Element (Key).Captured_Slot;
                              end if;
                              Ctx.Vars.Include (Key, VInfo);
                              if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                                 declare
                                    SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                    FP : constant String := Next_Reg (Ctx.E, "tcfp");
                                 begin
                                    Emit_Inst (Ctx.E,
                                      FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                      To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                    Emit_Inst (Ctx.E, "store.ptr " & Reg & ", " & FP & ", align 8");
                                 end;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;

            when DN_TASK_BODY =>
               declare
                  TB_Id : constant TREE := D (AS_SOURCE_NAME, Item);
                  TS    : constant TREE := (if not Is_Void_Tree (TB_Id) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TB_Id))) else CONST_VOID);
                  Did_Set : Boolean := False;
               begin
                  if not Is_Void_Tree (TS) and then Ctx.Tasks.Contains (Tree_Key (TS)) then
                     declare
                        TI : constant Task_Info := Ctx.Tasks.Element (Tree_Key (TS));
                     begin
                        if Length (TI.Elab_Flag) > 0 then
                           if TI.Elab_Is_Local then
                              if TI.Elab_Owner = Ctx.Cur_Subp_Key then
                                 Emit_Inst (Ctx.E, "store.i8 1, " & To_String (TI.Elab_Flag) & ", align 1");
                              end if;
                              if Length (TI.Elab_Fallback) > 0 then
                                 declare
                                    GA : constant String := Next_Reg (Ctx.E, "te1");
                                 begin
                                    Emit_Inst (Ctx.E, GA & " = addr " & To_String (TI.Elab_Fallback));
                                    Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                                 end;
                              end if;
                              Did_Set := True;
                           else
                              declare
                                 GA : constant String := Next_Reg (Ctx.E, "te1");
                              begin
                                 Emit_Inst (Ctx.E, GA & " = addr " & To_String (TI.Elab_Flag));
                                 Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                                 Did_Set := True;
                              end;
                           end if;
                        end if;
                     end;
                  end if;
                  if not Did_Set and then Ctx.Enable_Checks and then not Ctx.In_Subunit
                    and then not Is_Void_Tree (TB_Id) and then Is_Library_Level (TB_Id)
                  then
                     declare
                        GSym : constant String := "@ada_telab__" & Qualified_Ada_Name (TB_Id);
                        GA   : constant String := Next_Reg (Ctx.E, "te1");
                     begin
                        Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global i8, align 1");
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                     end;
                  end if;
               end;
               Emit_Task_Body (Ctx, Item);

            when DN_SUBPROGRAM_BODY =>
               declare
                  SID : constant TREE := Canonical_Def (Subst_Tree (Ctx, Canonical_Def (D (AS_SOURCE_NAME, Item))));
                  Bdy : constant TREE := D (AS_BODY, Item);
               begin
                  if KIND (SID) = DN_GENERIC_ID then
                     if Ctx.Gen_Elab_Flags.Contains (Tree_Key (SID)) then
                        declare
                           GSym : constant String := To_String (Ctx.Gen_Elab_Flags.Element (Tree_Key (SID)).Pir_Type);
                           GA   : constant String := Next_Reg (Ctx.E, "ge1");
                        begin
                           Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                           Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                        end;
                     elsif Ctx.Enable_Checks and then not Ctx.In_Subunit and then Is_Library_Level (SID) then
                        declare
                           GSym : constant String := "@ada_gelab__" & Qualified_Ada_Name (SID);
                           GA   : constant String := Next_Reg (Ctx.E, "ge1");
                        begin
                           Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global i8, align 1");
                           Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                           Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                        end;
                     end if;
                  else
                     declare
                        SK : constant Unsigned_32 := Tree_Key (SID);
                     begin
                        if Ctx.In_Lib_Pkg_Body and then Ctx.Enable_Checks and then not Ctx.In_Subunit
                          and then not Is_Void_Tree (Bdy) and then KIND (Bdy) /= DN_STUB
                        then
                           Pre_Register_Subp (Ctx, SID, D (AS_HEADER, Item), Ctx.Cur_Subp_Key,
                             Is_Subunit => (Ctx.In_Subunit or else Ctx.Has_Stubs));
                           declare
                              SI : Subp_Info := Ctx.Subps.Element (SK);
                           begin
                              if Length (SI.Elab_Flag) = 0 then
                                 declare
                                    GSym : constant String := "@ada_selab_" & Tree_Key_Str (SID) & To_String (Ctx.Gen_Inst_Tag);
                                 begin
                                    Add_Global_Decl (Ctx.E, GSym, GSym & " = global i8 = 0, align 1");
                                    SI.Elab_Flag := To_Unbounded_String (GSym);
                                    Ctx.Subps.Replace (SK, SI);
                                 end;
                              end if;
                           end;
                        end if;
                        if Ctx.Subps.Contains (SK) and then Length (Ctx.Subps.Element (SK).Elab_Flag) > 0 then
                           declare
                              GA : constant String := Next_Reg (Ctx.E, "se1");
                           begin
                              Emit_Inst (Ctx.E, GA & " = addr " & To_String (Ctx.Subps.Element (SK).Elab_Flag));
                              Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                           end;
                        end if;
                     end;
                     Emit_Subprogram_Body (Ctx, Item, Is_Exported => Export_All,
                       Is_Subunit => (Ctx.In_Subunit or else Ctx.Has_Stubs));
                  end if;
               end;

            when others =>
               null;
         end case;
      end loop;

      -- Emit generic instantiations now that generic bodies in this scope are registered
      for PI of Local_Insts loop
         Emit_Generic_Instantiation (Ctx, PI.Inst_Decl, Export_All);
      end loop;
   end Emit_Decl_Items;

   -- =========================================================================
   -- Statements
   -- =========================================================================

   procedure Emit_Stm_List (Ctx : in out Context; Stm_S : TREE) is
      L : SEQ_TYPE;
      S : TREE;
   begin
      if Is_Void_Tree (Stm_S) then
         return;
      end if;
      L := LIST (Stm_S);
      while not IS_EMPTY (L) loop
         POP (L, S);
         if not Is_Terminated (Ctx.E) or else KIND (S) = DN_LABELED then
            Emit_Stm (Ctx, S);
         end if;
      end loop;
   end Emit_Stm_List;

   procedure Emit_Exception_Handlers
     (Ctx        : in out Context;
      Alt_S      : TREE;
      Hdl_Label  : String;
      After_Lbl  : String)
   is
      Exn_Reg : constant String := Next_Reg (Ctx.E, "exn");
      Alts    : SEQ_TYPE := LIST (Alt_S);
      Alt     : TREE;
      Has_Others : Boolean := False;
      Top_Idx : constant Natural := Natural (Ctx.Handlers.Length) - 1;
   begin
      -- Mark handler entry as In_Handler so calls inside handler propagate to outer handler (not self!)
      declare
         HE : Handler_Entry := Ctx.Handlers.Element (Top_Idx);
      begin
         HE.In_Handler  := True;
         HE.Current_Exn := To_Unbounded_String (Exn_Reg);
         Ctx.Handlers.Replace_Element (Top_Idx, HE);
      end;

      Emit_Label (Ctx.E, Hdl_Label, Exn_Reg & ": ptr");

      -- LRM 9.10(6): beginning of an exception handler is a mandatory abort sync point
      declare
         Dummy_Ab : Unbounded_String;
      begin
         Emit_Call_Or_Invoke (Ctx, "@__ada_check_abort", "", "", Dummy_Ab, Nounwind => True);
      end;

      -- Reclaim any Secondary Stack temporaries & inner alloca_dyn allocations from the failed try body!
      declare
         HE : constant Handler_Entry := Ctx.Handlers.Element (Top_Idx);
         Dummy : Unbounded_String;
      begin
         if Length (HE.SS_Mark) > 0 then
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (HE.SS_Mark), "", Dummy, Nounwind => True);
         end if;
         if Length (HE.SP_Mark) > 0 then
            Emit_Inst (Ctx.E, "stackrestore " & To_String (HE.SP_Mark));
         end if;
      end;

      while not IS_EMPTY (Alts) loop
         POP (Alts, Alt);
         if KIND (Alt) = DN_ALTERNATIVE then
            declare
               CS       : SEQ_TYPE := LIST (D (AS_CHOICE_S, Alt));
               Ch       : TREE;
               Is_Oth   : Boolean := False;
               Cond_Reg : Unbounded_String;
            begin
               while not IS_EMPTY (CS) loop
                  POP (CS, Ch);
                  if KIND (Ch) = DN_CHOICE_OTHERS then
                     Is_Oth := True;
                     Has_Others := True;
                  elsif KIND (Ch) = DN_CHOICE_EXP then
                     declare
                        EN : TREE := D (AS_EXP, Ch);
                        ED : TREE;
                     begin
                        if KIND (EN) = DN_SELECTED then
                           EN := D (AS_DESIGNATOR, EN);
                        end if;
                        ED := D (SM_DEFN, EN);
                        declare
                           Tag : constant String := Get_Exc_Tag (Ctx, ED);
                           TA  : constant String := Next_Reg (Ctx.E, "et");
                           Eq  : constant String := Next_Reg (Ctx.E, "eq");
                        begin
                           Emit_Inst (Ctx.E, TA & " = addr " & Tag);
                           Emit_Inst (Ctx.E, Eq & " = cmp_eq.ptr " & Exn_Reg & ", " & TA);
                           if Length (Cond_Reg) = 0 then
                              Cond_Reg := To_Unbounded_String (Eq);
                           else
                              declare
                                 OrR : constant String := Next_Reg (Ctx.E, "eor");
                              begin
                                 Emit_Inst (Ctx.E, OrR & " = or.i8 " & To_String (Cond_Reg) & ", " & Eq);
                                 Cond_Reg := To_Unbounded_String (OrR);
                              end;
                           end if;
                        end;
                     end;
                  end if;
               end loop;

               if Is_Oth then
                  Emit_Stm_List (Ctx, D (AS_STM_S, Alt));
                  if not Is_Terminated (Ctx.E) then
                     Emit_Term (Ctx.E, "jmp " & After_Lbl);
                  end if;
               elsif Length (Cond_Reg) > 0 then
                  declare
                     L_Match : constant String := Next_Label (Ctx.E, "eh_m");
                     L_Next  : constant String := Next_Label (Ctx.E, "eh_n");
                  begin
                     Emit_Term (Ctx.E, "br " & To_String (Cond_Reg) & ", " & L_Match & ", " & L_Next);
                     Emit_Label (Ctx.E, L_Match);
                     Emit_Stm_List (Ctx, D (AS_STM_S, Alt));
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "jmp " & After_Lbl);
                     end if;
                     Emit_Label (Ctx.E, L_Next);
                  end;
               end if;
            end;
         end if;
      end loop;

      -- Unmatched exception: re-propagate to outer handler or caller
      if not Has_Others and then not Is_Terminated (Ctx.E) then
         Emit_Raise_Reg (Ctx, Exn_Reg);
      end if;
   end Emit_Exception_Handlers;

   function Type_Contains_Task (Ctx : in out Context; Raw_T : TREE; Depth : Natural := 0) return Boolean is
      R : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Raw_T));
   begin
      if Is_Void_Tree (R) or else Depth > 8 then
         return False;
      end if;
      if Is_Task_Type (R) then
         return True;
      elsif Is_Constrained_Array (R) or else KIND (R) = DN_ARRAY then
         declare
            BT : constant TREE := Subst_Base_Type (Ctx, R);
         begin
            return not Is_Void_Tree (BT) and then KIND (BT) = DN_ARRAY
              and then Type_Contains_Task (Ctx, D (SM_COMP_TYPE, BT), Depth + 1);
         end;
      elsif Is_Record_Type (R) then
         declare
            BT : constant TREE := Subst_Base_Type (Ctx, R);

            function Comp_List_Contains_Task (CL : TREE; Dp : Natural) return Boolean is
            begin
               if Is_Void_Tree (CL) or else Dp > 8 then
                  return False;
               end if;
               if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                  declare
                     DL   : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                     Decl : TREE;
                  begin
                     while not IS_EMPTY (DL) loop
                        POP (DL, Decl);
                        if KIND (Decl) = DN_VARIABLE_DECL then
                           declare
                              CID : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, Decl)));
                           begin
                              if Type_Contains_Task (Ctx, D (SM_OBJ_TYPE, CID), Dp + 1) then
                                 return True;
                              end if;
                           end;
                        end if;
                     end loop;
                  end;
               end if;
               declare
                  VP : constant TREE := D (AS_VARIANT_PART, CL);
               begin
                  if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART
                    and then not Is_Void_Tree (D (AS_VARIANT_S, VP))
                  then
                     declare
                        VL : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                        VN : TREE;
                     begin
                        while not IS_EMPTY (VL) loop
                           POP (VL, VN);
                           if KIND (VN) = DN_VARIANT
                             and then Comp_List_Contains_Task (D (AS_COMP_LIST, VN), Dp + 1)
                           then
                              return True;
                           end if;
                        end loop;
                     end;
                  end if;
               end;
               return False;
            end Comp_List_Contains_Task;
         begin
            if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
               if Comp_List_Contains_Task (D (SM_COMP_LIST, BT), Depth) then
                  return True;
               end if;
            end if;
         end;
      end if;
      return False;
   end Type_Contains_Task;

   function Item_List_Has_Tasks (Ctx : in out Context; Item_S : TREE) return Boolean is
      L  : SEQ_TYPE;
      It : TREE;
   begin
      if Is_Void_Tree (Item_S) then
         return False;
      end if;
      L := LIST (Item_S);
      while not IS_EMPTY (L) loop
         POP (L, It);
         case KIND (It) is
            when DN_TASK_DECL | DN_TASK_BODY =>
               return True;
            when DN_TYPE_DECL =>
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, It);
                  TS  : constant TREE :=
                    (if not Is_Void_Tree (TID) and then KIND (TID) in CLASS_TYPE_NAME
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TID))) else CONST_VOID);
               begin
                  if not Is_Void_Tree (TS) and then KIND (TS) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                    and then Type_Contains_Task (Ctx, D (SM_DESIG_TYPE, TS))
                  then
                     return True;
                  end if;
               end;
            when DN_VARIABLE_DECL =>
               declare
                  VID : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, It)));
               begin
                  if Type_Contains_Task (Ctx, D (SM_OBJ_TYPE, VID)) then
                     return True;
                  end if;
               end;
            when DN_PACKAGE_DECL =>
               declare
                  UK  : constant TREE := D (AS_UNIT_KIND, It);
                  Hdr : constant TREE :=
                    (if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION
                     then D (SM_SPEC, Canonical_Def (D (AS_SOURCE_NAME, It)))
                     else D (AS_HEADER, It));
               begin
                  if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                     if Item_List_Has_Tasks (Ctx, D (AS_DECL_S1, Hdr))
                       or else Item_List_Has_Tasks (Ctx, D (AS_DECL_S2, Hdr))
                     then
                        return True;
                     end if;
                  end if;
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     declare
                        Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                        GK     : constant Unsigned_32 := Tree_Key (Gen_Id);
                        GB     : TREE := CONST_VOID;
                     begin
                        if not Is_Void_Tree (Gen_Id) then
                           if Ctx.Generic_Body.Contains (GK) then
                              GB := Ctx.Generic_Body.Element (GK);
                           elsif KIND (Gen_Id) = DN_GENERIC_ID then
                              GB := D (SM_BODY, Gen_Id);
                              if Is_Void_Tree (GB) then
                                 GB := D (XD_BODY, Gen_Id);
                              end if;
                           end if;
                        end if;
                        if not Is_Void_Tree (GB) then
                           declare
                              BB : constant TREE := (if KIND (GB) = DN_PACKAGE_BODY then D (AS_BODY, GB) else GB);
                              Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
                              GDecl : constant TREE :=
                                (if Ctx.Generic_Decls.Contains (GK) then Ctx.Generic_Decls.Element (GK)
                                 else CONST_VOID);
                              Param_S : constant TREE :=
                                (if not Is_Void_Tree (GDecl) and then KIND (GDecl) = DN_GENERIC_DECL then D (AS_ITEM_S, GDecl)
                                 elsif not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_GENERIC_PARAM_S, Gen_Id)
                                 else CONST_VOID);
                              ADecls_N : constant TREE := D (SM_DECL_S, UK);
                              Res_Has : Boolean := False;
                           begin
                              -- Temporarily map formal types -> actual types so tasks via formal limited private types are detected!
                              if not Is_Void_Tree (Param_S) and then not Is_Void_Tree (ADecls_N) then
                                 declare
                                    FL : SEQ_TYPE := LIST (Param_S);
                                    AL : SEQ_TYPE := LIST (ADecls_N);
                                    FI, AD : TREE;
                                 begin
                                    while not IS_EMPTY (FL) and then not IS_EMPTY (AL) loop
                                       POP (FL, FI);
                                       if KIND (FI) in DN_IN | DN_IN_OUT then
                                          declare
                                             FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FI));
                                             Dummy_F : TREE;
                                          begin
                                             while not IS_EMPTY (FNS) and then not IS_EMPTY (AL) loop
                                                POP (FNS, Dummy_F);
                                                POP (AL, AD);
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
                                          end if;
                                       end if;
                                    end loop;
                                 end;
                              end if;
                              if not Is_Void_Tree (BB) and then KIND (BB) = DN_BLOCK_BODY then
                                 Res_Has := Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, BB));
                              end if;
                              Ctx.Generic_Subst := Saved_Sub;
                              if Res_Has then
                                 return True;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;
            when DN_PACKAGE_BODY =>
               declare
                  Bdy : constant TREE := D (AS_BODY, It);
               begin
                  if not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_BLOCK_BODY then
                     if Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Bdy)) then
                        return True;
                     end if;
                  elsif not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_STUB then
                     -- P3-4.9: Separate local package body subunit may declare tasks attached to enclosing master
                     return True;
                  end if;
               end;
            when others =>
               null;
         end case;
      end loop;
      return False;
   end Item_List_Has_Tasks;

   procedure Emit_Master_Leave_To (Ctx : in out Context; Target_Depth : Natural) is
      Dummy : Unbounded_String;
   begin
      if Natural (Ctx.Master_Stack.Length) > Target_Depth then
         for I in reverse Target_Depth .. Natural (Ctx.Master_Stack.Length) - 1 loop
            Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (Ctx.Master_Stack.Element (I)), "", Dummy);
         end loop;
      end if;
   end Emit_Master_Leave_To;

   procedure Scan_Label_Master_Depths
     (Ctx      : in out Context;
      Node     : TREE;
      Depth    : Natural;
      SS_Depth : Natural := 0;
      SP_Depth : Natural := 0)
   is
      K : NODE_NAME;
   begin
      if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then
         return;
      end if;
      K := KIND (Node);
      if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then
         return;
      end if;
      if K = DN_LABELED then
         declare
            LS  : constant TREE := D (AS_SOURCE_NAME_S, Node);
            LL  : SEQ_TYPE;
            LID : TREE;
         begin
            if not Is_Void_Tree (LS) then
               LL := LIST (LS);
               while not IS_EMPTY (LL) loop
                  POP (LL, LID);
                  Ctx.Label_Mst_Depths.Include (Tree_Key (LID), Integer_64 (Depth));
                  Ctx.Label_SS_Depths.Include (Tree_Key (LID), Integer_64 (SS_Depth));
                  Ctx.Label_SP_Depths.Include (Tree_Key (LID), Integer_64 (SP_Depth));
               end loop;
            end if;
         end;
         Scan_Label_Master_Depths (Ctx, D (AS_STM, Node), Depth, SS_Depth, SP_Depth);
         return;
      elsif K = DN_BLOCK then
         declare
            BB      : constant TREE := D (AS_BLOCK_BODY, Node);
            NextD   : Natural := Depth;
            Next_SS : Natural := SS_Depth;
            Next_SP : constant Natural := SP_Depth + 1;
         begin
            if not Is_Void_Tree (BB) then
               if Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, BB)) then
                  NextD := Depth + 1;
               end if;
               if Tree_Uses_SS (Ctx, BB) and then not (Ctx.Cur_Ret_Uncon and then Tree_Has_Return (BB)) then
                  Next_SS := SS_Depth + 1;
               end if;
               Scan_Label_Master_Depths (Ctx, D (AS_STM_S, BB), NextD, Next_SS, Next_SP);
               Scan_Label_Master_Depths (Ctx, D (AS_ALTERNATIVE_S, BB), NextD, Next_SS, Next_SP);
            end if;
         end;
         return;
      elsif K = DN_LOOP then
         declare
            Iter    : constant TREE := D (AS_ITERATION, Node);
            BStms   : constant TREE := D (AS_STM_S, Node);
            Cond_SS : constant Boolean :=
              not Is_Void_Tree (Iter) and then KIND (Iter) = DN_WHILE and then Tree_Uses_SS (Ctx, D (AS_EXP, Iter));
            Loop_SS : constant Boolean := Tree_Uses_SS (Ctx, BStms) or else Cond_SS;
            Next_SS : constant Natural := SS_Depth + (if Loop_SS then 1 else 0);
         begin
            Scan_Label_Master_Depths (Ctx, BStms, Depth, Next_SS, SP_Depth);
         end;
         return;
      end if;
      if ARITY (Node) = ARBITRARY then
         declare
            L    : SEQ_TYPE := LIST (Node);
            Item : TREE;
         begin
            while not IS_EMPTY (L) loop
               POP (L, Item);
               Scan_Label_Master_Depths (Ctx, Item, Depth, SS_Depth, SP_Depth);
            end loop;
         end;
      else
         for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
            Scan_Label_Master_Depths (Ctx, DABS (I, Node), Depth, SS_Depth, SP_Depth);
         end loop;
      end if;
   end Scan_Label_Master_Depths;

   procedure Emit_Block_Body
     (Ctx        : in out Context;
      Block_Node : TREE;
      Is_Subp    : Boolean)
   is
      Item_S    : constant TREE := D (AS_ITEM_S, Block_Node);
      Stm_S     : constant TREE := D (AS_STM_S, Block_Node);
      Alt_S     : constant TREE := D (AS_ALTERNATIVE_S, Block_Node);
      Has_Hdl   : constant Boolean := (not Is_Void_Tree (Alt_S)) and then (not IS_EMPTY (LIST (Alt_S)));
      Has_Tasks : constant Boolean := Item_List_Has_Tasks (Ctx, Item_S);
      Hdl_Lbl   : Unbounded_String;
      After_Lbl : Unbounded_String;
      Saved_SP  : Unbounded_String;
      Has_Dyn   : Boolean := False;
      Has_SS    : Boolean := False;
      Saved_SS  : Unbounded_String;
      Saved_Mst : constant Unbounded_String := Ctx.Cur_Master_Reg;
      Local_Mst : Unbounded_String;
   begin
      if not Is_Subp then
         declare
            SP : constant String := Next_Reg (Ctx.E, "sp");
         begin
            Emit_Inst (Ctx.E, SP & " = stacksave");
            Saved_SP := To_Unbounded_String (SP);
            Ctx.SP_Marks.Append (Saved_SP);
         end;
         if Tree_Uses_SS (Ctx, Block_Node)
           and then not (Ctx.Cur_Ret_Uncon and then Tree_Has_Return (Block_Node))
         then
            Has_SS := True;
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", Saved_SS, Nounwind => True);
            Ctx.SS_Marks.Append (Saved_SS);
         end if;
      end if;

      declare
         MDec_Exc : Unbounded_String;
         Mst_Exc  : Unbounded_String;
         Mst_Done : Unbounded_String;
      begin
         if Has_Tasks then
            Emit_Call_Or_Invoke (Ctx, "@__ada_master_enter", "", "ptr", Local_Mst);
            Ctx.Cur_Master_Reg := Local_Mst;
            Ctx.Master_Stack.Append (Local_Mst);
            MDec_Exc := To_Unbounded_String (Next_Label (Ctx.E, "mdec_exc"));
            Mst_Exc  := To_Unbounded_String (Next_Label (Ctx.E, "mst_exc"));
            Mst_Done := To_Unbounded_String (Next_Label (Ctx.E, "mst_done"));
            -- If an exception occurs during declaration elaboration, abort unactivated tasks and re-raise (LRM 9.3)
            Ctx.Handlers.Append ((Handler_Label => MDec_Exc,
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));
         end if;

         declare
            Block_Has_Stubs : Boolean := False;
            Saved_Has_Stubs : constant Boolean := Ctx.Has_Stubs;
         begin
            if Is_Subp and then not Is_Void_Tree (Item_S) then
               declare
                  SL : SEQ_TYPE := LIST (Item_S);
                  IT : TREE;
               begin
                  while not IS_EMPTY (SL) loop
                     POP (SL, IT);
                     if (KIND (IT) = DN_SUBPROGRAM_BODY or else KIND (IT) = DN_PACKAGE_BODY or else KIND (IT) = DN_TASK_BODY)
                       and then not Is_Void_Tree (D (AS_BODY, IT))
                       and then KIND (D (AS_BODY, IT)) = DN_STUB
                       and then not Is_Gen_Subunit_Stub (Ctx, IT)
                     then
                        Block_Has_Stubs := True;
                        exit;
                     end if;
                  end loop;
               end;
            end if;
            if Block_Has_Stubs then
               Ctx.Has_Stubs := True;
            end if;
            Emit_Decl_Items (Ctx, Item_S, Is_Global => Block_Has_Stubs, Export_All => Block_Has_Stubs, Has_Dyn => Has_Dyn);
            Ctx.Has_Stubs := Saved_Has_Stubs;
         end;

         if Has_Tasks then
            Ctx.Handlers.Delete_Last;
            -- Outer cleanup handler so ANY unhandled exception leaving the block waits for dependent tasks (LRM 9.4)
            Ctx.Handlers.Append ((Handler_Label => Mst_Exc,
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));
         end if;

         if Has_Hdl then
            Hdl_Lbl   := To_Unbounded_String (Next_Label (Ctx.E, "exc_h"));
            After_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "exc_end"));
            declare
               H_Mark : Unbounded_String := Null_Unbounded_String;
               H_SP   : constant String := Next_Reg (Ctx.E, "hsp");
            begin
               -- Take a fresh SS mark & SP mark AFTER Emit_Decl_Items so entering an exception handler
               -- never frees local dynamic/unconstrained arrays declared in this block/subprogram!
               Emit_Inst (Ctx.E, H_SP & " = stacksave");
               if not Ctx.Cur_Ret_Uncon and then Tree_Uses_SS (Ctx, Stm_S) then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", H_Mark, Nounwind => True);
               end if;
               Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                     Current_Exn   => Null_Unbounded_String,
                                     In_Handler    => False,
                                     SS_Mark       => H_Mark,
                                     SP_Mark       => To_Unbounded_String (H_SP)));
            end;
         end if;

         -- Activate all dependent tasks declared in this master scope right at `begin` (LRM 9.3)
         if Has_Tasks then
            declare
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate", To_String (Local_Mst), "", Dummy);
            end;
         end if;

         Emit_Stm_List (Ctx, Stm_S);

         if Has_Hdl then
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & To_String (After_Lbl));
            end if;
            Emit_Exception_Handlers (Ctx, Alt_S, To_String (Hdl_Lbl), To_String (After_Lbl));
            Ctx.Handlers.Delete_Last;
            Emit_Label (Ctx.E, To_String (After_Lbl));
         end if;

         if Has_Tasks then
            Ctx.Handlers.Delete_Last;
            if not Is_Terminated (Ctx.E) then
               declare
                  Dummy : Unbounded_String;
               begin
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (Local_Mst), "", Dummy);
                  Emit_Term (Ctx.E, "jmp " & To_String (Mst_Done));
               end;
            end if;
            declare
               Dex_R : constant String := Next_Reg (Ctx.E, "mdex");
               Mex_R : constant String := Next_Reg (Ctx.E, "msex");
               Dummy : Unbounded_String;
            begin
               Emit_Label (Ctx.E, To_String (MDec_Exc), Dex_R & ": ptr");
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_unactivated", To_String (Local_Mst), "", Dummy, Nounwind => True);
               Emit_Raise_Reg (Ctx, Dex_R);

               Emit_Label (Ctx.E, To_String (Mst_Exc), Mex_R & ": ptr");
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (Local_Mst), "", Dummy, Nounwind => True);
               Emit_Raise_Reg (Ctx, Mex_R);

               Emit_Label (Ctx.E, To_String (Mst_Done));
            end;
            Ctx.Cur_Master_Reg := Saved_Mst;
            if not Ctx.Master_Stack.Is_Empty then
               Ctx.Master_Stack.Delete_Last;
            end if;
         end if;
      end;

      if Has_SS then
         if not Is_Terminated (Ctx.E) then
            declare
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (Saved_SS), "", Dummy, Nounwind => True);
            end;
         end if;
         Ctx.SS_Marks.Delete_Last;
      end if;

      if not Is_Subp then
         if Has_Dyn and then not Is_Terminated (Ctx.E) then
            Emit_Inst (Ctx.E, "stackrestore " & To_String (Saved_SP));
         end if;
         Ctx.SP_Marks.Delete_Last;
      end if;
   end Emit_Block_Body;

   procedure Emit_Case_Stm (Ctx : in out Context; Stm : TREE) is
      Sel    : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Stm));
      PT     : constant String := To_String (Sel.Pir_Type);
      Alts   : SEQ_TYPE := LIST (D (AS_ALTERNATIVE_S, Stm));
      Alt    : TREE;
      L_End  : constant String := Next_Label (Ctx.E, "cas_end");
      L_Def  : constant String := Next_Label (Ctx.E, "cas_def");

      type Case_Arm is record
         Lbl  : Unbounded_String;
         Node : TREE;
      end record;
      package Arm_Vecs is new Ada.Containers.Vectors (Natural, Case_Arm);
      Arms     : Arm_Vecs.Vector;
      Sw_Cases : Unbounded_String;
      Has_Sw   : Boolean := False;
      Def_Arm  : TREE := CONST_VOID;

      type Wide_Range is record
         Lo  : Integer_64;
         Hi  : Integer_64;
         Lbl : Unbounded_String;
      end record;
      package WR_Vecs is new Ada.Containers.Vectors (Natural, Wide_Range);
      Wide_Ranges : WR_Vecs.Vector;
   begin
      while not IS_EMPTY (Alts) loop
         POP (Alts, Alt);
         if KIND (Alt) = DN_ALTERNATIVE then
            declare
               AL  : constant String := Next_Label (Ctx.E, "cas_a");
               CS  : SEQ_TYPE := LIST (D (AS_CHOICE_S, Alt));
               Ch  : TREE;
               Is_Oth : Boolean := False;
            begin
               while not IS_EMPTY (CS) loop
                  POP (CS, Ch);
                  if KIND (Ch) = DN_CHOICE_OTHERS then
                     Is_Oth := True;
                     Def_Arm := Alt;
                  elsif KIND (Ch) = DN_CHOICE_EXP then
                     declare
                        V : constant Integer_64 := Eval_Int64 (Safe_SM_Value (D (AS_EXP, Ch)));
                     begin
                        if Has_Sw then Append (Sw_Cases, ", "); end if;
                        Has_Sw := True;
                        Append (Sw_Cases, Img (Long_Long_Integer (V)) & " -> " & AL);
                     end;
                  elsif KIND (Ch) = DN_CHOICE_RANGE then
                     declare
                        DR : constant TREE := D (AS_DISCRETE_RANGE, Ch);
                        Lo, Hi : Integer_64 := 0;
                        St : Boolean := False;
                     begin
                        if KIND (DR) = DN_RANGE then
                           if Has_Static_Val (Safe_SM_Value (D (AS_EXP1, DR))) and then
                              Has_Static_Val (Safe_SM_Value (D (AS_EXP2, DR)))
                           then
                              Lo := Eval_Int64 (Safe_SM_Value (D (AS_EXP1, DR)));
                              Hi := Eval_Int64 (Safe_SM_Value (D (AS_EXP2, DR)));
                              St := True;
                           end if;
                        elsif KIND (DR) = DN_DISCRETE_SUBTYPE then
                           declare
                              SI : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                           begin
                              if KIND (SI) = DN_SUBTYPE_INDICATION and then not Is_Void_Tree (D (AS_CONSTRAINT, SI))
                                and then KIND (D (AS_CONSTRAINT, SI)) = DN_RANGE
                              then
                                 declare
                                    R : constant TREE := D (AS_CONSTRAINT, SI);
                                 begin
                                    if Has_Static_Val (Safe_SM_Value (D (AS_EXP1, R))) and then
                                       Has_Static_Val (Safe_SM_Value (D (AS_EXP2, R)))
                                    then
                                       Lo := Eval_Int64 (Safe_SM_Value (D (AS_EXP1, R)));
                                       Hi := Eval_Int64 (Safe_SM_Value (D (AS_EXP2, R)));
                                       St := True;
                                    end if;
                                 end;
                              else
                                 declare
                                    Def : constant TREE :=
                                      (if KIND (SI) = DN_SUBTYPE_INDICATION then Get_Name_Defn (D (AS_NAME, SI))
                                       else Get_Name_Defn (SI));
                                 begin
                                    if not Is_Void_Tree (Def) and then KIND (Def) in CLASS_TYPE_NAME then
                                       Get_Discrete_Bounds (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def)), St, Lo, Hi);
                                    end if;
                                 end;
                              end if;
                           end;
                        end if;
                        if St then
                           if Hi >= Lo and then Lo <= Integer_64'Last - 64 and then Hi <= Lo + 64 then
                              for V in Lo .. Hi loop
                                 if Has_Sw then Append (Sw_Cases, ", "); end if;
                                 Has_Sw := True;
                                 Append (Sw_Cases, Img (Long_Long_Integer (V)) & " -> " & AL);
                              end loop;
                           elsif Hi >= Lo then
                              Wide_Ranges.Append ((Lo => Lo, Hi => Hi, Lbl => To_Unbounded_String (AL)));
                           end if;
                        end if;
                     end;
                  end if;
               end loop;
               if not Is_Oth then
                  Arms.Append ((Lbl => To_Unbounded_String (AL), Node => Alt));
               end if;
            end;
         end if;
      end loop;

      if Has_Sw then
         Emit_Term (Ctx.E,
           "switch." & PT & " " & To_String (Sel.Reg) & ", default " & L_Def &
           " [ " & To_String (Sw_Cases) & " ]");
      else
         Emit_Term (Ctx.E, "jmp " & L_Def);
      end if;

      for A of Arms loop
         Emit_Label (Ctx.E, To_String (A.Lbl));
         Emit_Stm_List (Ctx, D (AS_STM_S, A.Node));
         if not Is_Terminated (Ctx.E) then
            Emit_Term (Ctx.E, "jmp " & L_End);
         end if;
      end loop;

      Emit_Label (Ctx.E, L_Def);
      declare
         Is_Uns : constant Boolean := Is_Unsigned_Scalar (Subst_Tree (Ctx, Sel.Ada_Type), PT);
         Ge_Op  : constant String := (if Is_Uns then "cmp_ge_u." else "cmp_ge_s.");
         Le_Op  : constant String := (if Is_Uns then "cmp_le_u." else "cmp_le_s.");
      begin
         for WR of Wide_Ranges loop
            declare
               C1   : constant String := Next_Reg (Ctx.E, "wc1");
               C2   : constant String := Next_Reg (Ctx.E, "wc2");
               InR  : constant String := Next_Reg (Ctx.E, "win");
               Next : constant String := Next_Label (Ctx.E, "wnx");
            begin
               Emit_Inst (Ctx.E, C1 & " = " & Ge_Op & PT & " " & To_String (Sel.Reg) & ", " & Img (Long_Long_Integer (WR.Lo)));
               Emit_Inst (Ctx.E, C2 & " = " & Le_Op & PT & " " & To_String (Sel.Reg) & ", " & Img (Long_Long_Integer (WR.Hi)));
               Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
               Emit_Term (Ctx.E, "br " & InR & ", " & To_String (WR.Lbl) & ", " & Next);
               Emit_Label (Ctx.E, Next);
            end;
         end loop;
      end;

      if not Is_Void_Tree (Def_Arm) then
         Emit_Stm_List (Ctx, D (AS_STM_S, Def_Arm));
      end if;
      if not Is_Terminated (Ctx.E) then
         Emit_Term (Ctx.E, "jmp " & L_End);
      end if;
      Emit_Label (Ctx.E, L_End);
   end Emit_Case_Stm;

   procedure Emit_Loop_Stm (Ctx : in out Context; Stm : TREE) is
      Iter      : constant TREE := D (AS_ITERATION, Stm);
      Loop_Id   : constant TREE := D (AS_SOURCE_NAME, Stm);
      L_Key     : constant Unsigned_32 := (if Is_Void_Tree (Loop_Id) then 0 else Tree_Key (Loop_Id));
      L_End     : constant String := Next_Label (Ctx.E, "lp_end");
      Body_Stms : constant TREE := D (AS_STM_S, Stm);
      Cond_SS   : constant Boolean :=
        not Is_Void_Tree (Iter) and then KIND (Iter) = DN_WHILE and then Tree_Uses_SS (Ctx, D (AS_EXP, Iter));
      Loop_SS   : constant Boolean := Tree_Uses_SS (Ctx, Body_Stms) or else Cond_SS;

      procedure Emit_Body_With_SS is
         Mark  : Unbounded_String;
         Dummy : Unbounded_String;
      begin
         if Loop_SS then
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", Mark, Nounwind => True);
            Ctx.SS_Marks.Append (Mark);
         end if;
         Emit_Stm_List (Ctx, Body_Stms);
         if Loop_SS then
            if not Is_Terminated (Ctx.E) then
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (Mark), "", Dummy, Nounwind => True);
            end if;
            Ctx.SS_Marks.Delete_Last;
         end if;
      end Emit_Body_With_SS;
   begin
      Ctx.Loops.Append ((Label_Key    => L_Key,
                         Exit_Label   => To_Unbounded_String (L_End),
                         SS_Depth     => Natural (Ctx.SS_Marks.Length),
                         Master_Depth => Natural (Ctx.Master_Stack.Length),
                         SP_Depth     => Natural (Ctx.SP_Marks.Length)));

      if Is_Void_Tree (Iter) then
         -- Infinite loop
         declare
            L_Top : constant String := Next_Label (Ctx.E, "lp_top");
         begin
            Emit_Term (Ctx.E, "jmp " & L_Top);
            Emit_Label (Ctx.E, L_Top);
            Emit_Body_With_SS;
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & L_Top);
            end if;
         end;

      elsif KIND (Iter) = DN_WHILE then
         declare
            L_Cond : constant String := Next_Label (Ctx.E, "wh_c");
            L_Body : constant String := Next_Label (Ctx.E, "wh_b");
         begin
            Emit_Term (Ctx.E, "jmp " & L_Cond);
            Emit_Label (Ctx.E, L_Cond);
            declare
               CMark : Unbounded_String;
               Dummy : Unbounded_String;
            begin
               if Cond_SS then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", CMark, Nounwind => True);
               end if;
               declare
                  C  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Iter));
                  CR : constant String := Emit_Scalar_Coerce (Ctx, To_String (C.Reg), To_String (C.Pir_Type), "i8");
               begin
                  if Cond_SS then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (CMark), "", Dummy, Nounwind => True);
                  end if;
                  Emit_Term (Ctx.E, "br " & CR & ", " & L_Body & ", " & L_End);
               end;
            end;
            Emit_Label (Ctx.E, L_Body);
            Emit_Body_With_SS;
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & L_Cond);
            end if;
         end;

      elsif KIND (Iter) = DN_FOR or else KIND (Iter) = DN_REVERSE then
         declare
            Is_Rev   : constant Boolean := KIND (Iter) = DN_REVERSE;
            Iter_Id  : constant TREE := D (AS_SOURCE_NAME, Iter);
            Rng_Node : constant TREE := D (AS_DISCRETE_RANGE, Iter);
            PT, Lo_R, Hi_R : Unbounded_String;
            Start_R, Stop_R : Unbounded_String;
            Empty_C  : constant String := Next_Reg (Ctx.E, "femp");
            L_Body   : constant String := Next_Label (Ctx.E, "for_b");
            L_Step   : constant String := Next_Label (Ctx.E, "for_s");
            Var_Reg  : constant String := Next_Reg (Ctx.E, "iv");
            Cur_V    : constant String := Next_Reg (Ctx.E, "fv");
            Done_C   : constant String := Next_Reg (Ctx.E, "fdone");
            Next_V   : constant String := Next_Reg (Ctx.E, "fnx");
            Ada_T    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Iter_Id));
            TI       : constant Type_Info := Get_Type_Info (Ctx, Ada_T);
            VInfo    : Var_Info;
         begin
            Emit_Discrete_Range (Ctx, Rng_Node, PT, Lo_R, Hi_R);
            -- Allocate loop variable in .entry (promotable to SSA register by mem2reg!)
            Emit_Entry_Alloca (Ctx.E, Var_Reg & " = alloca " & To_String (PT) & ", align " & Img (TI.Align));
            VInfo.Kind       := VK_Local_Alloca;
            VInfo.Pir_Name   := To_Unbounded_String (Var_Reg);
            VInfo.Pir_Type   := PT;
            VInfo.Ada_Type   := Ada_T;
            VInfo.Align      := TI.Align;
            VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
            -- Record static loop bounds on read-only Ada loop parameter for sound bounds-check elimination!
            if KIND (Rng_Node) = DN_RANGE then
               declare
                  V1 : constant TREE := Safe_SM_Value (D (AS_EXP1, Rng_Node));
                  V2 : constant TREE := Safe_SM_Value (D (AS_EXP2, Rng_Node));
               begin
                  if Has_Static_Val (V1) and then Has_Static_Val (V2) then
                     VInfo.Has_Static_Rng := True;
                     VInfo.Rng_Lo := Eval_Int64 (V1);
                     VInfo.Rng_Hi := Eval_Int64 (V2);
                  end if;
               end;
            end if;
            if Ctx.Vars.Contains (Tree_Key (Iter_Id)) and then Ctx.Vars.Element (Tree_Key (Iter_Id)).Owner_Subp = Ctx.Cur_Subp_Key then
               VInfo.Captured_Slot := Ctx.Vars.Element (Tree_Key (Iter_Id)).Captured_Slot;
            end if;
            Ctx.Vars.Include (Tree_Key (Iter_Id), VInfo);
            if VInfo.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
               declare
                  SI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                  FP : constant String := Next_Reg (Ctx.E, "lcfp");
               begin
                  Emit_Inst (Ctx.E,
                    FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                    To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                  Emit_Inst (Ctx.E, "store.ptr " & Var_Reg & ", " & FP & ", align 8");
               end;
            end if;

            Start_R := (if Is_Rev then Hi_R else Lo_R);
            Stop_R  := (if Is_Rev then Lo_R else Hi_R);

            -- Check if upper/lower bound is statically safe from wrap (enables canonical 2-block loop for loop_vec/rotate!)
            declare
               Can_Fast : Boolean := False;
               Limit_V  : Integer_64 := 0;
               Is_Uns   : constant Boolean := Is_Unsigned_Scalar (Ada_T, To_String (PT));
            begin
               if KIND (Rng_Node) = DN_RANGE then
                  declare
                     Bound_N : constant TREE := (if Is_Rev then D (AS_EXP1, Rng_Node) else D (AS_EXP2, Rng_Node));
                     BV      : constant TREE := Safe_SM_Value (Bound_N);
                     Bits    : constant Natural := Scalar_Bits (Ada_T);
                  begin
                     if Has_Static_Val (BV) then
                        Limit_V := Eval_Int64 (BV);
                        if Is_Uns then
                           if not Is_Rev then
                              if (Bits = 8 and then Limit_V < 255)
                                or else (Bits = 16 and then Limit_V < 65535)
                                or else (Bits = 32 and then Limit_V < 2147483647)
                              then
                                 Can_Fast := True;
                              end if;
                           else
                              if Limit_V > 0 then
                                 Can_Fast := True;
                              end if;
                           end if;
                        elsif not Is_Rev then
                           if (Bits = 32 and then Limit_V < 2147483647)
                             or else (Bits = 64 and then Limit_V < Integer_64'Last)
                             or else (Bits = 16 and then Limit_V < 32767)
                             or else (Bits = 8 and then Limit_V < 127)
                           then
                              Can_Fast := True;
                           end if;
                        else
                           if (Bits = 32 and then Limit_V > -2147483648)
                             or else (Bits = 64 and then Limit_V > Integer_64'First)
                             or else (Bits = 16 and then Limit_V > -32768)
                             or else (Bits = 8 and then Limit_V > -128)
                           then
                              Can_Fast := True;
                           end if;
                        end if;
                     end if;
                  end;
               end if;

               if Can_Fast then
                  declare
                     L_Hdr  : constant String := Next_Label (Ctx.E, "for_h");
                     Cmp_C  : constant String := Next_Reg (Ctx.E, "fcmp");
                     Cmp_Op : constant String :=
                       (if Is_Uns then (if Is_Rev then "cmp_ge_u." else "cmp_le_u.")
                        else (if Is_Rev then "cmp_ge_s." else "cmp_le_s."));
                     Step_Fl : constant String := (if Is_Uns then "" else " [unsafe_s]");
                  begin
                     Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & To_String (Start_R) & ")");
                     Emit_Label (Ctx.E, L_Hdr, Cur_V & ": " & To_String (PT));
                     Emit_Inst (Ctx.E, Cmp_C & " = " & Cmp_Op & To_String (PT) & " " & Cur_V & ", " & To_String (Stop_R));
                     Emit_Term (Ctx.E, "br " & Cmp_C & ", " & L_Body & ", " & L_End);
                     Emit_Label (Ctx.E, L_Body);
                     Emit_Inst (Ctx.E, "store." & To_String (PT) & " " & Cur_V & ", " & Var_Reg & ", align " & Img (TI.Align));
                     Emit_Body_With_SS;
                     if not Is_Terminated (Ctx.E) then
                        if Is_Rev then
                           Emit_Inst (Ctx.E, Next_V & " = sub." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        else
                           Emit_Inst (Ctx.E, Next_V & " = add." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        end if;
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Next_V & ")");
                     end if;
                  end;
               else
                  -- Wrap-safe loop when bound is dynamic or equals Type'Last / Type'First!
                  declare
                     Gt_Op   : constant String := (if Is_Uns then "cmp_gt_u." else "cmp_gt_s.");
                     Step_Fl : constant String := (if Is_Uns then "" else " [unsafe_s]");
                  begin
                     Emit_Inst (Ctx.E, Empty_C & " = " & Gt_Op & To_String (PT) & " " & To_String (Lo_R) & ", " & To_String (Hi_R));
                     Emit_Term (Ctx.E, "br " & Empty_C & ", " & L_End & ", " & L_Body & "(" & To_String (Start_R) & ")");

                     Emit_Label (Ctx.E, L_Body, Cur_V & ": " & To_String (PT));
                     Emit_Inst (Ctx.E, "store." & To_String (PT) & " " & Cur_V & ", " & Var_Reg & ", align " & Img (TI.Align));
                     Emit_Body_With_SS;
                     if not Is_Terminated (Ctx.E) then
                        Emit_Inst (Ctx.E, Done_C & " = cmp_eq." & To_String (PT) & " " & Cur_V & ", " & To_String (Stop_R));
                        Emit_Term (Ctx.E, "br " & Done_C & ", " & L_End & ", " & L_Step);
                        Emit_Label (Ctx.E, L_Step);
                        if Is_Rev then
                           Emit_Inst (Ctx.E, Next_V & " = sub." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        else
                           Emit_Inst (Ctx.E, Next_V & " = add." & To_String (PT) & " " & Cur_V & ", 1" & Step_Fl);
                        end if;
                        Emit_Term (Ctx.E, "jmp " & L_Body & "(" & Next_V & ")");
                     end if;
                  end;
               end if;
            end;
         end;
      end if;

      Ctx.Loops.Delete_Last;
      Emit_Label (Ctx.E, L_End);
   end Emit_Loop_Stm;

   procedure Emit_Stm (Ctx : in out Context; Stm : TREE) is
      K : constant NODE_NAME := KIND (Stm);
   begin
      case K is
         when DN_NULL_STM | DN_STM_PRAGMA =>
            null;

         when DN_LABELED =>
            declare
               LS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Stm));
               LID : TREE;
            begin
               while not IS_EMPTY (LS) loop
                  POP (LS, LID);
                  declare
                     Lbl : constant String := ".ada_lbl_" & Tree_Key_Str (LID) & To_String (Ctx.Gen_Inst_Tag);
                  begin
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "jmp " & Lbl);
                     end if;
                     Emit_Label (Ctx.E, Lbl);
                  end;
               end loop;
               Emit_Stm (Ctx, D (AS_STM, Stm));
            end;

         when DN_GOTO =>
            declare
               LDef : constant TREE := Get_Name_Defn (D (AS_NAME, Stm));
               LK   : constant Unsigned_32 := Tree_Key (LDef);
            begin
               if Ctx.Label_Mst_Depths.Contains (LK) then
                  Emit_Master_Leave_To (Ctx, Natural (Ctx.Label_Mst_Depths.Element (LK)));
               end if;
               if Ctx.Label_SS_Depths.Contains (LK) then
                  Emit_SS_Release_To (Ctx, Natural (Ctx.Label_SS_Depths.Element (LK)));
               end if;
               if Ctx.Label_SP_Depths.Contains (LK) then
                  Emit_SP_Restore_To (Ctx, Natural (Ctx.Label_SP_Depths.Element (LK)));
               end if;
               Emit_Term (Ctx.E, "jmp .ada_lbl_" & Tree_Key_Str (LDef) & To_String (Ctx.Gen_Inst_Tag));
            end;

         when DN_ASSIGN =>
            declare
               LHS_Node  : constant TREE := D (AS_NAME, Stm);
               Is_BF_LHS : Boolean := False;
               BF_FE     : Field_Entry;
               BF_Def    : TREE := CONST_VOID;
            begin
               if KIND (LHS_Node) = DN_USED_OBJECT_ID
                 or else (KIND (LHS_Node) = DN_SELECTED
                          and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))
                          and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))))
                                   not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID)
               then
                  declare
                     OD_L : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (LHS_Node)));
                  begin
                     if not Is_Void_Tree (OD_L) and then Ctx.Vars.Contains (Tree_Key (OD_L))
                       and then Ctx.Vars.Element (Tree_Key (OD_L)).Is_Bitfield
                     then
                        Is_BF_LHS := True;
                        BF_FE     := Ctx.Vars.Element (Tree_Key (OD_L)).BF_FE;
                        BF_Def    := OD_L;
                     end if;
                  end;
               elsif KIND (LHS_Node) = DN_SELECTED then
                  declare
                     Des : TREE := D (AS_DESIGNATOR, LHS_Node);
                     Def : TREE;
                  begin
                     while not Is_Void_Tree (Des) and then KIND (Des) = DN_SELECTED loop
                        Des := D (AS_DESIGNATOR, Des);
                     end loop;
                     Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des)));
                     if not Is_Void_Tree (Def) and then KIND (Def) in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                        declare
                           Pfx_E  : constant TREE := D (AS_NAME, LHS_Node);
                           Pfx_Ts : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                           Rec_Ts : constant TREE :=
                             (if Is_Access_Type (Pfx_Ts) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_Ts)))
                              else Pfx_Ts);
                           Dummy_RTI : constant Type_Info := Get_Type_Info (Ctx, Rec_Ts);
                           pragma Unreferenced (Dummy_RTI);
                           CK : constant Unsigned_32 := Tree_Key (Def);
                        begin
                           if Ctx.Fields.Contains (CK) and then Is_Bitfield (Ctx.Fields.Element (CK))
                             and then Get_Type_Info (Ctx, Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def))).Kind = TK_Scalar
                           then
                              Is_BF_LHS := True;
                              BF_FE     := Ctx.Fields.Element (CK);
                              BF_Def    := Def;
                           end if;
                        end;
                     end if;
                  end;
               end if;
               if Is_BF_LHS then
                  declare
                     Is_Ren_BF : constant Boolean := KIND (BF_Def) not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID;
                     Rec_P  : Unbounded_String;
                     BF_CT  : constant TREE :=
                       (if Is_Ren_BF then Ctx.Vars.Element (Tree_Key (BF_Def)).Ada_Type
                        else Subst_Tree (Ctx, D (SM_OBJ_TYPE, BF_Def)));
                     DTI    : constant Type_Info := Get_Type_Info (Ctx, BF_CT);
                     RHS    : Expr_Val;
                     VR     : Unbounded_String;
                  begin
                     if Is_Ren_BF then
                        Rec_P := To_Unbounded_String (Emit_Var_Addr (Ctx, Ctx.Vars.Element (Tree_Key (BF_Def))));
                     else
                        declare
                           Pfx_LV : constant Expr_Val := Emit_LValue (Ctx, D (AS_NAME, LHS_Node));
                           Pfx_T  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Pfx_LV.Ada_Type));
                        begin
                           Rec_P := Pfx_LV.Reg;
                           if Is_Access_Type (Pfx_T) then
                              if Pfx_LV.Kind = EK_Addr then
                                 declare
                                    LP : constant String := Next_Reg (Ctx.E, "bfap");
                                 begin
                                    Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_P) & ", align 8");
                                    Rec_P := To_Unbounded_String (LP);
                                 end;
                              end if;
                              Emit_Null_Check (Ctx, To_String (Rec_P));
                           end if;
                           -- P3-6.3: Run variant discriminant check before bitfield store (LRM 4.1.3(16))
                           if BF_FE.In_Union then
                              declare
                                 Rec_T_BF : constant TREE :=
                                   (if Is_Access_Type (Pfx_T) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T)))
                                    else Pfx_T);
                              begin
                                 Emit_Variant_Check_For_Field (Ctx, To_String (Rec_P), Rec_T_BF, BF_Def, Tree_Key (BF_Def));
                              end;
                           end if;
                        end;
                     end if;
                     RHS := Emit_Expr (Ctx, D (AS_EXP, Stm), BF_CT);
                     VR  := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (RHS.Reg), To_String (RHS.Pir_Type), To_String (DTI.Pir_Type)));
                     Emit_Range_Check (Ctx, To_String (VR), To_String (DTI.Pir_Type), BF_CT);
                     Emit_Bitfield_Store (Ctx, To_String (Rec_P), BF_FE, To_String (VR), To_String (DTI.Pir_Type), BF_CT);
                  end;
               else
                  declare
                     LHS       : constant Expr_Val := Emit_LValue (Ctx, LHS_Node);
                     DTI       : constant Type_Info := Get_Type_Info (Ctx, LHS.Ada_Type);
                     RHS_Node  : constant TREE := D (AS_EXP, Stm);
                     Is_Shared : constant Boolean := Is_Shared_Object (Ctx, LHS_Node);
                  begin
               if DTI.Kind = TK_Scalar then
                  declare
                     RHS : constant Expr_Val := Emit_Expr (Ctx, RHS_Node, LHS.Ada_Type);
                     SP  : constant String := To_String (RHS.Pir_Type);
                     DP  : constant String := To_String (DTI.Pir_Type);
                     VR  : Unbounded_String;
                     -- P3-3.13: If LHS is an object with its own per-object frozen range (multi-object decl),
                     -- bind its frozen range into Saved_Type_Ranges for the duration of Emit_Range_Check!
                     Swap_TK  : Unsigned_32 := 0;
                     Had_Orig : Boolean := False;
                     Orig_SR  : Saved_Range_Info;
                  begin
                     if KIND (LHS_Node) = DN_USED_OBJECT_ID
                       or else (KIND (LHS_Node) = DN_SELECTED
                                and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))
                                and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, LHS_Node)))))
                                         not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID)
                     then
                        declare
                           OD_A : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (LHS_Node)));
                           OK_A : constant Unsigned_32 := (if not Is_Void_Tree (OD_A) then Tree_Key (OD_A) else 0);
                           RT_A : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, LHS.Ada_Type));
                        begin
                           if OK_A /= 0 and then Ctx.Saved_Var_Ranges.Contains (OK_A) and then not Is_Void_Tree (RT_A) then
                              Swap_TK := Tree_Key (RT_A);
                              if Ctx.Saved_Type_Ranges.Contains (Swap_TK) then
                                 Had_Orig := True;
                                 Orig_SR  := Ctx.Saved_Type_Ranges.Element (Swap_TK);
                              end if;
                              Ctx.Saved_Type_Ranges.Include (Swap_TK, Ctx.Saved_Var_Ranges.Element (OK_A));
                           end if;
                        end;
                     end if;
                     if Int_Bits_Of_Pir (SP) > Int_Bits_Of_Pir (DP) and then Is_Discrete_Type (LHS.Ada_Type) then
                        Emit_Range_Check (Ctx, To_String (RHS.Reg), SP, LHS.Ada_Type);
                     end if;
                     VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (RHS.Reg), SP, DP));
                     Emit_Range_Check (Ctx, To_String (VR), DP, LHS.Ada_Type);
                     if Swap_TK /= 0 then
                        if Had_Orig then
                           Ctx.Saved_Type_Ranges.Include (Swap_TK, Orig_SR);
                        else
                           Ctx.Saved_Type_Ranges.Exclude (Swap_TK);
                        end if;
                     end if;
                     Emit_Discriminant_Check (Ctx, To_String (VR), LHS.Ada_Type, Is_Access => True);
                     if Is_Shared then
                        Emit_Inst (Ctx.E, "fence [seq_cst]");
                        Emit_Inst (Ctx.E,
                          "store." & DP & " " & To_String (VR) &
                          ", " & To_String (LHS.Reg) & ", align " & Img (DTI.Align) & " [volatile]");
                        Emit_Inst (Ctx.E, "fence [seq_cst]");
                     else
                        Emit_Inst (Ctx.E,
                          "store." & DP & " " & To_String (VR) &
                          ", " & To_String (LHS.Reg) & ", align " & Img (DTI.Align));
                     end if;
                  end;
               else
                  declare
                     -- Check if RHS is an aggregate containing `others` (within array dims only!) assigned to an
                     -- unconstrained/dynamic array LHS (needs LHS's bounds in a temp buffer
                     -- to preserve LRM 5.2 exception safety & non-aliasing!).
                     function Agg_Has_Others (AN : TREE; Rem_D : Natural) return Boolean is
                     begin
                        if Rem_D = 0 or else Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then
                           return False;
                        end if;
                        declare
                           AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                           IT : TREE;
                        begin
                           while not IS_EMPTY (AL) loop
                              POP (AL, IT);
                              if KIND (IT) = DN_ASSOC or else KIND (IT) = DN_NAMED then
                                 declare
                                    CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                    Ch : TREE;
                                 begin
                                    while not IS_EMPTY (CS) loop
                                       POP (CS, Ch);
                                       if KIND (Ch) = DN_CHOICE_OTHERS then
                                          return True;
                                       end if;
                                    end loop;
                                 end;
                                 if Rem_D > 1 and then Agg_Has_Others (D (AS_EXP, IT), Rem_D - 1) then
                                    return True;
                                 end if;
                              else
                                 if Rem_D > 1 and then Agg_Has_Others (IT, Rem_D - 1) then
                                    return True;
                                 end if;
                              end if;
                           end loop;
                        end;
                        return False;
                     end Agg_Has_Others;

                     RHS : Expr_Val;
                     Is_Null_Str : Boolean := False;
                  begin
                     if KIND (RHS_Node) = DN_AGGREGATE
                       and then (DTI.Kind = TK_Unconstrained_Array
                                 or else (DTI.Kind = TK_Constrained_Array and then DTI.Is_Dynamic))
                       and then Agg_Has_Others (RHS_Node, Natural'Max (1, DTI.NDims))
                     then
                        -- Allocate temp buffer with LHS's bounds, initialize temp, then copy to LHS
                        declare
                           ETI   : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
                           EAl   : constant Natural := Elem_Storage_Align (ETI);
                           ESz   : constant Natural := Elem_Storage_Size (ETI);
                           ND    : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
                           LFs, LLs : Str_Array_4;
                           Tot64 : Unbounded_String;
                           Byt   : Unbounded_String;
                           Buf_U : Unbounded_String;
                        begin
                           for D_I in 1 .. ND loop
                              Get_Array_Dim_Bounds (Ctx, LHS, D_I, LFs (D_I), LLs (D_I));
                           end loop;
                           Emit_Array_Byte_Size_I64 (Ctx, LFs, LLs, ND, ESz, Tot64, Byt);
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", To_String (Byt) & ", " & Img (EAl), "ptr", Buf_U);
                           Emit_Init_Composite (Ctx, To_String (Buf_U), LHS.Ada_Type, RHS_Node, LFs, LLs);
                           RHS.Kind       := EK_Slice;
                           RHS.Reg        := Buf_U;
                           RHS.Pir_Type   := To_Unbounded_String ("ptr");
                           RHS.Ada_Type   := LHS.Ada_Type;
                           RHS.Align      := EAl;
                           RHS.NDims      := ND;
                           RHS.First_Regs := LFs;
                           RHS.Last_Regs  := LLs;
                        end;
                     elsif KIND (RHS_Node) = DN_STRING_LITERAL
                       and then Sym_Name (RHS_Node) = """"""
                       and then (DTI.Kind = TK_Constrained_Array or else DTI.Kind = TK_Unconstrained_Array)
                     then
                        -- Null string literal "" assigned to array/slice LHS (LRM 4.2(4))
                        Is_Null_Str := True;
                        declare
                           Z1 : constant String := Next_Reg (Ctx.E, "nsf");
                           Z2 : constant String := Next_Reg (Ctx.E, "nsl");
                           ZP : constant String := Next_Reg (Ctx.E, "nsp");
                        begin
                           Emit_Inst (Ctx.E, Z1 & " = const.i32 1");
                           Emit_Inst (Ctx.E, Z2 & " = const.i32 0");
                           Emit_Inst (Ctx.E, ZP & " = const.ptr 0");
                           RHS.Kind := EK_Slice;
                           RHS.Reg := To_Unbounded_String (ZP);
                           RHS.Pir_Type := To_Unbounded_String ("ptr");
                           RHS.Ada_Type := LHS.Ada_Type;
                           RHS.NDims := 1;
                           RHS.First_Regs (1) := To_Unbounded_String (Z1);
                           RHS.Last_Regs (1) := To_Unbounded_String (Z2);
                        end;
                     else
                        RHS := Emit_Expr (Ctx, RHS_Node, LHS.Ada_Type);
                     end if;

                     -- Length check across ALL dimensions (with Ada 83 null-to-null allowance)
                     if DTI.Kind = TK_Constrained_Array or else DTI.Kind = TK_Unconstrained_Array then
                        Emit_Array_Length_Check (Ctx, LHS, RHS, Natural'Max (1, DTI.NDims));
                     end if;
                     if DTI.Kind = TK_Record then
                        Emit_Discriminant_Check (Ctx, To_String (RHS.Reg), LHS.Ada_Type, Is_Access => False);
                        -- LRM 3.7.2(4) / 5.2(4) (P3-3.1, P3-3.3): DN_ALL dereference, constrained record components
                        -- (including discriminant-dependent O.Inner := Bad!), records without default discriminants,
                        -- and constrained IN OUT/OUT/renamed records must preserve LHS's existing discriminants!
                        if Ctx.Enable_Checks then
                           declare
                              LHS_RT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, LHS.Ada_Type));
                              BT : constant TREE := Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, LHS.Ada_Type)));
                              DS : constant TREE := (if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then D (SM_DISCRIMINANT_S, BT) else CONST_VOID);
                              Must_Match : Boolean :=
                                KIND (D (AS_NAME, Stm)) = DN_ALL
                                or else (not Is_Void_Tree (LHS_RT) and then KIND (LHS_RT) = DN_CONSTRAINED_RECORD);
                              Dyn_Con_Reg : Unbounded_String;
                           begin
                              if not Must_Match
                                and then (KIND (D (AS_NAME, Stm)) = DN_USED_OBJECT_ID
                                          or else (KIND (D (AS_NAME, Stm)) = DN_SELECTED
                                                   and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, D (AS_NAME, Stm))))
                                                   and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, D (AS_NAME, Stm))))))
                                                            not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID))
                              then
                                 declare
                                    LDef : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, Stm))));
                                 begin
                                    if not Is_Void_Tree (LDef) and then Ctx.Vars.Contains (Tree_Key (LDef)) then
                                       declare
                                          LVInfo : constant Var_Info := Ctx.Vars.Element (Tree_Key (LDef));
                                       begin
                                          if LVInfo.Owner_Subp = Ctx.Cur_Subp_Key and then Length (LVInfo.Constrained_Reg) > 0 then
                                             Dyn_Con_Reg := LVInfo.Constrained_Reg;
                                          elsif Length (LVInfo.Con_Global) > 0 then
                                             declare
                                                CGA : constant String := Next_Reg (Ctx.E, "dcga");
                                                CGV : constant String := Next_Reg (Ctx.E, "dcgv");
                                             begin
                                                Emit_Inst (Ctx.E, CGA & " = addr " & To_String (LVInfo.Con_Global));
                                                Emit_Inst (Ctx.E, CGV & " = load.i8 " & CGA & ", align 1");
                                                Dyn_Con_Reg := To_Unbounded_String (CGV);
                                             end;
                                          elsif LVInfo.Con_Captured_Slot > 0 and then LVInfo.Owner_Subp /= 0 and then LVInfo.Owner_Subp /= Ctx.Cur_Subp_Key then
                                             declare
                                                CFP : constant String := Emit_Captured_Slot_Ptr (Ctx, LVInfo.Owner_Subp, LVInfo.Con_Captured_Slot);
                                                CCP : constant String := Next_Reg (Ctx.E, "dccp");
                                                CCV : constant String := Next_Reg (Ctx.E, "dccv");
                                             begin
                                                Emit_Inst (Ctx.E, CCP & " = load.ptr " & CFP & ", align 8");
                                                Emit_Inst (Ctx.E, CCV & " = load.i8 " & CCP & ", align 1");
                                                Dyn_Con_Reg := To_Unbounded_String (CCV);
                                             end;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                              if not Must_Match and then Length (Dyn_Con_Reg) = 0
                                and then not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS))
                              then
                                 -- Records without default discriminants are always constrained
                                 declare
                                    DL : SEQ_TYPE := LIST (DS);
                                    DD : TREE;
                                 begin
                                    POP (DL, DD);
                                    if KIND (DD) = DN_DSCRMT_DECL and then Is_Void_Tree (D (AS_EXP, DD)) then
                                       declare
                                          NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                          DID : TREE;
                                       begin
                                          if not IS_EMPTY (NS) then
                                             POP (NS, DID);
                                             if Is_Void_Tree (D (SM_INIT_EXP, DID)) then
                                                Must_Match := True;
                                             end if;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                              if (Must_Match or else Length (Dyn_Con_Reg) > 0)
                                and then not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS))
                              then
                                 declare
                                    RTI    : constant Type_Info := Get_Type_Info (Ctx, BT);
                                    DL     : SEQ_TYPE := LIST (DS);
                                    DD     : TREE;
                                    FIdx   : Natural := 0;
                                    L_Skip : Unbounded_String;
                                 begin
                                    if not Must_Match and then Length (Dyn_Con_Reg) > 0 then
                                       declare
                                          L_Do : constant String := Next_Label (Ctx.E, "dcd");
                                       begin
                                          L_Skip := To_Unbounded_String (Next_Label (Ctx.E, "dcs"));
                                          Emit_Term (Ctx.E, "br " & To_String (Dyn_Con_Reg) & ", " & L_Do & ", " & To_String (L_Skip));
                                          Emit_Label (Ctx.E, L_Do);
                                       end;
                                    end if;
                                    while not IS_EMPTY (DL) loop
                                       POP (DL, DD);
                                       if KIND (DD) = DN_DSCRMT_DECL then
                                          declare
                                             NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                             DID : TREE;
                                          begin
                                             while not IS_EMPTY (NS) loop
                                                POP (NS, DID);
                                                declare
                                                   DT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                                   DTI_D : constant Type_Info := Get_Type_Info (Ctx, DT);
                                                   DK_D : constant Unsigned_32 := Tree_Key (Canonical_Def (DID));
                                                   LV, RV : Unbounded_String;
                                                   Neq : constant String := Next_Reg (Ctx.E, "dne");
                                                begin
                                                   if Ctx.Fields.Contains (DK_D) then
                                                      declare
                                                         DFE : constant Field_Entry := Ctx.Fields.Element (DK_D);
                                                      begin
                                                         if Is_Bitfield (DFE) then
                                                            LV := To_Unbounded_String (Emit_Bitfield_Load (Ctx, To_String (LHS.Reg), DFE, DT));
                                                            RV := To_Unbounded_String (Emit_Bitfield_Load (Ctx, To_String (RHS.Reg), DFE, DT));
                                                         elsif DFE.In_Union or else DFE.Has_Explicit_Offset then
                                                            declare
                                                               Off_R : constant String := Next_Reg (Ctx.E, "doff");
                                                               LP    : constant String := Next_Reg (Ctx.E, "dlp");
                                                               RP    : constant String := Next_Reg (Ctx.E, "drp");
                                                               LV0   : constant String := Next_Reg (Ctx.E, "dlv");
                                                               RV0   : constant String := Next_Reg (Ctx.E, "drv");
                                                            begin
                                                               Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (DFE.Byte_Offset));
                                                               Emit_Inst (Ctx.E, LP & " = ptr_add.i64 " & To_String (LHS.Reg) & ", " & Off_R & " [inbounds]");
                                                               Emit_Inst (Ctx.E, LV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & LP & ", align " & Img (Explicit_Field_Align (DFE, DTI_D.Align)));
                                                               Emit_Inst (Ctx.E, RP & " = ptr_add.i64 " & To_String (RHS.Reg) & ", " & Off_R & " [inbounds]");
                                                               Emit_Inst (Ctx.E, RV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & RP & ", align " & Img (Explicit_Field_Align (DFE, DTI_D.Align)));
                                                               LV := To_Unbounded_String (LV0);
                                                               RV := To_Unbounded_String (RV0);
                                                            end;
                                                         else
                                                            declare
                                                               LP  : constant String := Next_Reg (Ctx.E, "dlp");
                                                               RP  : constant String := Next_Reg (Ctx.E, "drp");
                                                               LV0 : constant String := Next_Reg (Ctx.E, "dlv");
                                                               RV0 : constant String := Next_Reg (Ctx.E, "drv");
                                                            begin
                                                               Emit_Inst (Ctx.E, LP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (LHS.Reg) & ", " & Img (DFE.Field_Idx));
                                                               Emit_Inst (Ctx.E, LV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & LP & ", align " & Img (DTI_D.Align));
                                                               Emit_Inst (Ctx.E, RP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (RHS.Reg) & ", " & Img (DFE.Field_Idx));
                                                               Emit_Inst (Ctx.E, RV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & RP & ", align " & Img (DTI_D.Align));
                                                               LV := To_Unbounded_String (LV0);
                                                               RV := To_Unbounded_String (RV0);
                                                            end;
                                                         end if;
                                                      end;
                                                   else
                                                      declare
                                                         LP  : constant String := Next_Reg (Ctx.E, "dlp");
                                                         RP  : constant String := Next_Reg (Ctx.E, "drp");
                                                         LV0 : constant String := Next_Reg (Ctx.E, "dlv");
                                                         RV0 : constant String := Next_Reg (Ctx.E, "drv");
                                                      begin
                                                         Emit_Inst (Ctx.E, LP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (LHS.Reg) & ", " & Img (FIdx));
                                                         Emit_Inst (Ctx.E, LV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & LP & ", align " & Img (DTI_D.Align));
                                                         Emit_Inst (Ctx.E, RP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (RHS.Reg) & ", " & Img (FIdx));
                                                         Emit_Inst (Ctx.E, RV0 & " = load." & To_String (DTI_D.Pir_Type) & " " & RP & ", align " & Img (DTI_D.Align));
                                                         LV := To_Unbounded_String (LV0);
                                                         RV := To_Unbounded_String (RV0);
                                                      end;
                                                   end if;
                                                   Emit_Inst (Ctx.E, Neq & " = cmp_ne." & To_String (DTI_D.Pir_Type) & " " & To_String (LV) & ", " & To_String (RV));
                                                   Emit_Check_Fail_If (Ctx, Neq);
                                                end;
                                                FIdx := FIdx + 1;
                                             end loop;
                                          end;
                                       end if;
                                    end loop;
                                    if Length (L_Skip) > 0 then
                                       Emit_Term (Ctx.E, "jmp " & To_String (L_Skip));
                                       Emit_Label (Ctx.E, To_String (L_Skip));
                                    end if;
                                 end;
                              end if;
                           end;
                        end if;
                     end if;
                     if not Is_Null_Str then
                        Emit_Copy_Composite (Ctx, To_String (LHS.Reg), RHS, LHS.Ada_Type);
                     end if;
                  end;
               end if;
                  end;
               end if;
            end;

         when DN_IF =>
            declare
               Clauses : SEQ_TYPE := LIST (D (AS_TEST_CLAUSE_ELEM_S, Stm));
               Clause  : TREE;
               Else_S  : constant TREE := D (AS_STM_S, Stm);
               L_End   : constant String := Next_Label (Ctx.E, "if_end");
            begin
               while not IS_EMPTY (Clauses) loop
                  POP (Clauses, Clause);
                  if KIND (Clause) = DN_COND_CLAUSE then
                     declare
                        Cond  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Clause));
                        CondR : constant String := Emit_Scalar_Coerce (Ctx, To_String (Cond.Reg), To_String (Cond.Pir_Type), "i8");
                        L_T   : constant String := Next_Label (Ctx.E, "if_t");
                        L_F   : constant String := Next_Label (Ctx.E, "if_f");
                     begin
                        Emit_Term (Ctx.E, "br " & CondR & ", " & L_T & ", " & L_F);
                        Emit_Label (Ctx.E, L_T);
                        Emit_Stm_List (Ctx, D (AS_STM_S, Clause));
                        if not Is_Terminated (Ctx.E) then
                           Emit_Term (Ctx.E, "jmp " & L_End);
                        end if;
                        Emit_Label (Ctx.E, L_F);
                     end;
                  end if;
               end loop;
               Emit_Stm_List (Ctx, Else_S);
               if not Is_Terminated (Ctx.E) then
                  Emit_Term (Ctx.E, "jmp " & L_End);
               end if;
               Emit_Label (Ctx.E, L_End);
            end;

         when DN_CASE =>
            Emit_Case_Stm (Ctx, Stm);

         when DN_LOOP =>
            Emit_Loop_Stm (Ctx, Stm);

         when DN_EXIT =>
            declare
               Target_Loop  : constant TREE := D (SM_STM, Stm);
               Cond_Exp     : constant TREE := D (AS_EXP, Stm);
               Exit_Lbl     : Unbounded_String;
               Exit_SS_Dep  : Natural := Natural (Ctx.SS_Marks.Length);
               Exit_Mst_Dep : Natural := Natural (Ctx.Master_Stack.Length);
               Exit_SP_Dep  : Natural := Natural (Ctx.SP_Marks.Length);
            begin
               if not Is_Void_Tree (Target_Loop) then
                  declare
                     LID : constant TREE := D (AS_SOURCE_NAME, Target_Loop);
                     LK  : constant Unsigned_32 := (if Is_Void_Tree (LID) then 0 else Tree_Key (LID));
                  begin
                     for I in reverse 0 .. Natural (Ctx.Loops.Length) - 1 loop
                        if LK /= 0 and then Ctx.Loops.Element (I).Label_Key = LK then
                           Exit_Lbl     := Ctx.Loops.Element (I).Exit_Label;
                           Exit_SS_Dep  := Ctx.Loops.Element (I).SS_Depth;
                           Exit_Mst_Dep := Ctx.Loops.Element (I).Master_Depth;
                           Exit_SP_Dep  := Ctx.Loops.Element (I).SP_Depth;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;
               if Length (Exit_Lbl) = 0 and then not Ctx.Loops.Is_Empty then
                  Exit_Lbl     := Ctx.Loops.Last_Element.Exit_Label;
                  Exit_SS_Dep  := Ctx.Loops.Last_Element.SS_Depth;
                  Exit_Mst_Dep := Ctx.Loops.Last_Element.Master_Depth;
                  Exit_SP_Dep  := Ctx.Loops.Last_Element.SP_Depth;
               end if;

               if Is_Void_Tree (Cond_Exp) then
                  Emit_Master_Leave_To (Ctx, Exit_Mst_Dep);
                  Emit_SS_Release_To (Ctx, Exit_SS_Dep);
                  Emit_SP_Restore_To (Ctx, Exit_SP_Dep);
                  Emit_Term (Ctx.E, "jmp " & To_String (Exit_Lbl));
               else
                  declare
                     C     : constant Expr_Val := Emit_Expr (Ctx, Cond_Exp);
                     CR    : constant String := Emit_Scalar_Coerce (Ctx, To_String (C.Reg), To_String (C.Pir_Type), "i8");
                     L_Ex  : constant String := Next_Label (Ctx.E, "ex_t");
                     L_Cnt : constant String := Next_Label (Ctx.E, "ex_c");
                  begin
                     if Natural (Ctx.SS_Marks.Length) > Exit_SS_Dep
                       or else Natural (Ctx.Master_Stack.Length) > Exit_Mst_Dep
                       or else Natural (Ctx.SP_Marks.Length) > Exit_SP_Dep
                     then
                        Emit_Term (Ctx.E, "br " & CR & ", " & L_Ex & ", " & L_Cnt);
                        Emit_Label (Ctx.E, L_Ex);
                        Emit_Master_Leave_To (Ctx, Exit_Mst_Dep);
                        Emit_SS_Release_To (Ctx, Exit_SS_Dep);
                        Emit_SP_Restore_To (Ctx, Exit_SP_Dep);
                        Emit_Term (Ctx.E, "jmp " & To_String (Exit_Lbl));
                        Emit_Label (Ctx.E, L_Cnt);
                     else
                        Emit_Term (Ctx.E, "br " & CR & ", " & To_String (Exit_Lbl) & ", " & L_Cnt);
                        Emit_Label (Ctx.E, L_Cnt);
                     end if;
                  end;
               end if;
            end;

         when DN_BLOCK =>
            Emit_Block_Body (Ctx, D (AS_BLOCK_BODY, Stm), Is_Subp => False);

         when DN_RETURN =>
            declare
               Exp : constant TREE := D (AS_EXP, Stm);

               procedure Emit_Copy_Outs is
                  Saved_Handlers : constant Handler_Vecs.Vector := Ctx.Handlers;
               begin
                  Emit_Master_Leave_To (Ctx, 0);
                  -- LRM 6.4.1 / 11.4.1: copy-out constraint checks propagate to caller, not callee handlers
                  Ctx.Handlers.Clear;
                  for CO of Ctx.Copy_Outs loop
                     if To_String (CO.Pir_Type) = "@__ada_arr_desc" then
                        Emit_Inst (Ctx.E, "memcpy " & To_String (CO.Ptr_Reg) & ", " & To_String (CO.Loc_Reg) & ", " & Img (Arr_Desc_Size) & ", align 8");
                     else
                        declare
                           V : constant String := Next_Reg (Ctx.E, "co");
                        begin
                           Emit_Inst (Ctx.E,
                             V & " = load." & To_String (CO.Pir_Type) & " " &
                             To_String (CO.Loc_Reg) & ", align " & Img (CO.Align));
                           Emit_Range_Check (Ctx, V, To_String (CO.Pir_Type), CO.Ada_Type);
                           Emit_Inst (Ctx.E,
                             "store." & To_String (CO.Pir_Type) & " " & V &
                             ", " & To_String (CO.Ptr_Reg) & ", align " & Img (CO.Align));
                        end;
                     end if;
                  end loop;
                  Ctx.Handlers := Saved_Handlers;
               end Emit_Copy_Outs;
            begin
               if Is_Void_Tree (Exp) then
                  if not Ctx.Accept_End_Lbls.Is_Empty then
                     if not Ctx.Accept_Mst_Depths.Is_Empty then
                        Emit_Master_Leave_To (Ctx, Natural (Ctx.Accept_Mst_Depths.Last_Element));
                     end if;
                     Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Accept_End_Lbls.Last_Element));
                  else
                     Emit_Copy_Outs;
                     Emit_SS_Release_To (Ctx, 0);
                     Emit_Term (Ctx.E, "ret");
                  end if;
               elsif Ctx.Cur_Ret_Uncon then
                  -- Unconstrained or dynamic-bound array return -> allocate result on Secondary Stack
                  -- and populate caller's descriptor { ptr data, i32 f1, i32 l1, ... } at %__ret!
                  declare
                     RTI     : constant Type_Info := Get_Type_Info (Ctx, Ctx.Cur_Ret_Type);
                     ETI     : constant Type_Info := Get_Type_Info (Ctx, RTI.Elem_Type);
                     EAl     : constant Natural := Elem_Storage_Align (ETI);
                     ESz     : constant Natural := Elem_Storage_Size (ETI);
                     ND      : constant Natural := Natural'Max (1, Natural'Min ((if RTI.NDims > 0 then RTI.NDims else 1), Max_Array_Dims));
                     Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, ND);
                     Firsts  : Str_Array_4;
                     Lasts   : Str_Array_4;
                     Tot64   : Unbounded_String;
                     SS_Ptr  : Unbounded_String;
                     Unw_Exp : TREE := Exp;
                     Ret_Tmp_Rng_Keys : U32_Vecs.Vector;
                     Ret_Tmp_Exp_Keys : U32_Vecs.Vector;

                     function Ret_Agg_Named_No_Others (AN : TREE; Rem_D : Natural) return Boolean is
                     begin
                        if Rem_D = 0 then return True; end if;
                        if Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then return False; end if;
                        declare
                           AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                           IT : TREE;
                           Has_Any : Boolean := False;
                        begin
                           while not IS_EMPTY (AL) loop
                              POP (AL, IT);
                              Has_Any := True;
                              if KIND (IT) not in DN_ASSOC | DN_NAMED then return False; end if;
                              declare
                                 CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                 Ch : TREE;
                              begin
                                 while not IS_EMPTY (CS) loop
                                    POP (CS, Ch);
                                    if KIND (Ch) = DN_CHOICE_OTHERS then return False; end if;
                                 end loop;
                              end;
                              if Rem_D > 1 and then not Ret_Agg_Named_No_Others (D (AS_EXP, IT), Rem_D - 1) then
                                 return False;
                              end if;
                           end loop;
                           return Has_Any;
                        end;
                     end Ret_Agg_Named_No_Others;
                  begin
                     -- P3-3.6: Do NOT strip DN_QUALIFIED (so `return S2'(others => 42)` enforces S2's bounds!)
                     while not Is_Void_Tree (Unw_Exp)
                       and then KIND (Unw_Exp) = DN_PARENTHESIZED
                     loop
                        Unw_Exp := D (AS_EXP, Unw_Exp);
                     end loop;

                     -- P3-3.7: For constrained return of named aggregate without `others`, fall into general branch to slide!
                     if KIND (Unw_Exp) = DN_AGGREGATE
                       and then not (RTI.Kind = TK_Constrained_Array and then Ret_Agg_Named_No_Others (Unw_Exp, ND))
                     then
                        -- Determine bounds of returned array aggregate (1D positional, named range, or dynamic subtype)
                        if RTI.Kind = TK_Constrained_Array then
                           declare
                              Sub_S : constant TREE := D (SM_INDEX_SUBTYPE_S, Resolve_Type_Spec (Ctx.Cur_Ret_Type));
                              IL    : SEQ_TYPE := LIST (Sub_S);
                              Idx_T : TREE;
                              Dim   : Natural := 0;
                           begin
                              while not IS_EMPTY (IL) and then Dim < ND loop
                                 POP (IL, Idx_T);
                                 Dim := Dim + 1;
                                 declare
                                    RT, RL, RH : Unbounded_String;
                                 begin
                                    Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (Idx_T)), RT, RL, RH);
                                    Firsts (Dim) := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                                    Lasts (Dim)  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                                 end;
                              end loop;
                           end;
                        else
                           -- Unconstrained array aggregate (1D..8D): infer bounds recursively per dimension!
                           declare
                              Cur_Agg : TREE := Unw_Exp;
                           begin
                              for Dim_I in 1 .. ND loop
                                 declare
                                    Assocs : SEQ_TYPE := (if not Is_Void_Tree (Cur_Agg) and then KIND (Cur_Agg) = DN_AGGREGATE
                                                          then LIST (D (AS_GENERAL_ASSOC_S, Cur_Agg)) else MAKE);
                                    Item   : TREE;
                                    Count  : Integer_64 := 0;
                                    Got_R  : Boolean := False;
                                    Next_A : TREE := CONST_VOID;
                                 begin
                                    if not Is_Void_Tree (Cur_Agg) and then KIND (Cur_Agg) = DN_STRING_LITERAL then
                                       Count := Integer_64 (Decode_Ada_String_Lit (Sym_Name (Cur_Agg))'Length);
                                    end if;
                                    while not IS_EMPTY (Assocs) loop
                                       POP (Assocs, Item);
                                       if KIND (Item) = DN_ASSOC or else KIND (Item) = DN_NAMED then
                                          if Is_Void_Tree (Next_A) then
                                             Next_A := D (AS_EXP, Item);
                                          end if;
                                          declare
                                             CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, Item));
                                             Ch : TREE;
                                          begin
                                             while not IS_EMPTY (CS) loop
                                                POP (CS, Ch);
                                                if KIND (Ch) = DN_CHOICE_RANGE then
                                                   declare
                                                      RT, RL, RH : Unbounded_String;
                                                      L32, H32 : Unbounded_String;
                                                      SR : Saved_Range_Info;
                                                   begin
                                                      Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                                      L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                                                      H32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                                                      -- P3-3.8: Cache Pass-1 choice range so Pass 2 does NOT double-evaluate!
                                                      Save_Scalar_Slot (Ctx, To_String (L32), "i32", 4, SR.Lo_Ptr, SR.Is_Global);
                                                      Save_Scalar_Slot (Ctx, To_String (H32), "i32", 4, SR.Hi_Ptr, SR.Is_Global);
                                                      SR.Pir_Type   := To_Unbounded_String ("i32");
                                                      SR.Align      := 4;
                                                      SR.Owner_Subp := Ctx.Cur_Subp_Key;
                                                      Ctx.Saved_Type_Ranges.Include (Tree_Key (Ch), SR);
                                                      Ret_Tmp_Rng_Keys.Append (Tree_Key (Ch));
                                                      if not Got_R then
                                                         Firsts (Dim_I) := L32;
                                                         Lasts (Dim_I)  := H32;
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "rmn");
                                                            ML : constant String := Next_Reg (Ctx.E, "rlo");
                                                            CH : constant String := Next_Reg (Ctx.E, "rmx");
                                                            MH : constant String := Next_Reg (Ctx.E, "rhi");
                                                         begin
                                                            Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i32 " & To_String (L32) & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, ML & " = select.i32 " & CL & ", " & To_String (L32) & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i32 " & To_String (H32) & ", " & To_String (Lasts (Dim_I)));
                                                            Emit_Inst (Ctx.E, MH & " = select.i32 " & CH & ", " & To_String (H32) & ", " & To_String (Lasts (Dim_I)));
                                                            Firsts (Dim_I) := To_Unbounded_String (ML);
                                                            Lasts (Dim_I)  := To_Unbounded_String (MH);
                                                         end;
                                                      end if;
                                                   end;
                                                elsif KIND (Ch) = DN_CHOICE_EXP then
                                                   declare
                                                      ChE : constant TREE := D (AS_EXP, Ch);
                                                      CE  : constant Expr_Val := Emit_Expr (Ctx, ChE);
                                                      C32 : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i32");
                                                      SE  : Saved_Expr_Info;
                                                   begin
                                                      if CE.Kind = EK_Scalar then
                                                         Save_Scalar_Slot (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), CE.Align, SE.Ptr_Reg, SE.Is_Global);
                                                         SE.Pir_Type   := CE.Pir_Type;
                                                         SE.Ada_Type   := CE.Ada_Type;
                                                         SE.Align      := CE.Align;
                                                         SE.Owner_Subp := Ctx.Cur_Subp_Key;
                                                         Ctx.Saved_Expr_Vals.Include (Tree_Key (ChE), SE);
                                                         Ret_Tmp_Exp_Keys.Append (Tree_Key (ChE));
                                                      end if;
                                                      if not Got_R then
                                                         Firsts (Dim_I) := To_Unbounded_String (C32);
                                                         Lasts (Dim_I)  := To_Unbounded_String (C32);
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "rmn");
                                                            ML : constant String := Next_Reg (Ctx.E, "rlo");
                                                            CH : constant String := Next_Reg (Ctx.E, "rmx");
                                                            MH : constant String := Next_Reg (Ctx.E, "rhi");
                                                         begin
                                                            Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i32 " & C32 & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, ML & " = select.i32 " & CL & ", " & C32 & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i32 " & C32 & ", " & To_String (Lasts (Dim_I)));
                                                            Emit_Inst (Ctx.E, MH & " = select.i32 " & CH & ", " & C32 & ", " & To_String (Lasts (Dim_I)));
                                                            Firsts (Dim_I) := To_Unbounded_String (ML);
                                                            Lasts (Dim_I)  := To_Unbounded_String (MH);
                                                         end;
                                                      end if;
                                                   end;
                                                end if;
                                             end loop;
                                          end;
                                       else
                                          if Is_Void_Tree (Next_A) then
                                             Next_A := Item;
                                          end if;
                                          Count := Count + 1;
                                       end if;
                                    end loop;
                                    if not Got_R then
                                       declare
                                          Idx_Lo_R, Idx_Hi_R : Unbounded_String;
                                          Lo64 : constant String := Next_Reg (Ctx.E, "rl64");
                                          Hi64 : constant String := Next_Reg (Ctx.E, "rh64");
                                          En64 : constant String := Next_Reg (Ctx.E, "re64");
                                          L32  : constant String := Next_Reg (Ctx.E, "ral");
                                       begin
                                          Emit_Index_Bounds (Ctx, RTI.Dims (Dim_I).Idx_Type, Idx_Lo_R, Idx_Hi_R);
                                          Emit_Inst (Ctx.E, Lo64 & " = sext.i64 " & To_String (Idx_Lo_R));
                                          Emit_Inst (Ctx.E, Hi64 & " = sext.i64 " & To_String (Idx_Hi_R));
                                          Emit_Inst (Ctx.E, En64 & " = add.i64 " & Lo64 & ", " & Img (Long_Long_Integer (Count - 1)));
                                          if Ctx.Enable_Checks and then Count > 0 then
                                             declare
                                                Oob : constant String := Next_Reg (Ctx.E, "raoob");
                                             begin
                                                Emit_Inst (Ctx.E, Oob & " = cmp_gt_s.i64 " & En64 & ", " & Hi64);
                                                Emit_Check_Fail_If (Ctx, Oob);
                                             end;
                                          end if;
                                          Emit_Inst (Ctx.E, L32 & " = trunc.i32 " & En64);
                                          Firsts (Dim_I) := Idx_Lo_R;
                                          Lasts (Dim_I)  := To_Unbounded_String (L32);
                                       end;
                                    end if;
                                    Cur_Agg := Next_A;
                                    while not Is_Void_Tree (Cur_Agg)
                                      and then (KIND (Cur_Agg) = DN_PARENTHESIZED or else KIND (Cur_Agg) = DN_QUALIFIED)
                                    loop
                                       Cur_Agg := D (AS_EXP, Cur_Agg);
                                    end loop;
                                 end;
                              end loop;
                           end;
                        end if;

                        declare
                           Byt : Unbounded_String;
                        begin
                           Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND, ESz, Tot64, Byt);
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", To_String (Byt) & ", " & Img (EAl), "ptr", SS_Ptr);
                        end;
                        Emit_Init_Composite (Ctx, To_String (SS_Ptr), Ctx.Cur_Ret_Type, Unw_Exp, Firsts, Lasts);
                        for K_Tmp of Ret_Tmp_Rng_Keys loop
                           Ctx.Saved_Type_Ranges.Exclude (K_Tmp);
                        end loop;
                        for K_Tmp of Ret_Tmp_Exp_Keys loop
                           Ctx.Saved_Expr_Vals.Exclude (K_Tmp);
                        end loop;
                     else
                        -- General array expression: evaluate, check/slide if constrained return type, allocate on SS, copy!
                        declare
                           Eval_RT : constant TREE :=
                             (if KIND (Unw_Exp) = DN_AGGREGATE and then RTI.Kind = TK_Constrained_Array
                              then Subst_Base_Type (Ctx, Ctx.Cur_Ret_Type)
                              else Ctx.Cur_Ret_Type);
                           Src : constant Expr_Val := Emit_Expr (Ctx, Exp, Eval_RT);
                           Byt : constant String := Next_Reg (Ctx.E, "rby");
                        begin
                           if RTI.Kind = TK_Constrained_Array then
                              declare
                                 Tgt_V : Expr_Val;
                              begin
                                 Tgt_V.Kind     := EK_Slice;
                                 Tgt_V.Ada_Type := Ctx.Cur_Ret_Type;
                                 Tgt_V.NDims    := ND;
                                 for D_Idx in 1 .. ND loop
                                    Emit_Index_Bounds (Ctx, RTI.Dims (D_Idx).Idx_Type, Firsts (D_Idx), Lasts (D_Idx));
                                    Tgt_V.First_Regs (D_Idx) := Firsts (D_Idx);
                                    Tgt_V.Last_Regs (D_Idx)  := Lasts (D_Idx);
                                 end loop;
                                 Emit_Array_Length_Check (Ctx, Tgt_V, Src, ND);
                              end;
                           else
                              for D_Idx in 1 .. ND loop
                                 Get_Array_Dim_Bounds (Ctx, Src, D_Idx, Firsts (D_Idx), Lasts (D_Idx));
                              end loop;
                           end if;
                           for D_Idx in 1 .. ND loop
                              declare
                                 C6 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_Idx)), To_String (Lasts (D_Idx)));
                              begin
                                 if D_Idx = 1 then
                                    Tot64 := To_Unbounded_String (C6);
                                 else
                                    declare
                                       M : constant String := Next_Reg (Ctx.E, "rm");
                                    begin
                                       Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & C6);
                                       Tot64 := To_Unbounded_String (M);
                                    end;
                                 end if;
                              end;
                           end loop;
                           Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Byt & ", " & Img (ETI.Align), "ptr", SS_Ptr);
                           Emit_Inst (Ctx.E, "memcpy " & To_String (SS_Ptr) & ", " & To_String (Src.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                        end;
                     end if;

                     -- Populate caller's descriptor struct at %__ret
                     declare
                        PF : constant String := Next_Reg (Ctx.E, "retpf");
                     begin
                        Emit_Inst (Ctx.E, PF & " = field_ptr " & Desc_Ty & ", " & To_String (Ctx.Cur_Ret_Ptr) & ", 0");
                        Emit_Inst (Ctx.E, "store.ptr " & To_String (SS_Ptr) & ", " & PF & ", align 8");
                        for D_Idx in 1 .. ND loop
                           declare
                              FF : constant String := Next_Reg (Ctx.E, "retff");
                              LF : constant String := Next_Reg (Ctx.E, "retlf");
                           begin
                              Emit_Inst (Ctx.E, FF & " = field_ptr " & Desc_Ty & ", " & To_String (Ctx.Cur_Ret_Ptr) & ", " & Img (2 * D_Idx - 1));
                              Emit_Inst (Ctx.E, "store.i32 " & To_String (Firsts (D_Idx)) & ", " & FF & ", align 4");
                              Emit_Inst (Ctx.E, LF & " = field_ptr " & Desc_Ty & ", " & To_String (Ctx.Cur_Ret_Ptr) & ", " & Img (2 * D_Idx));
                              Emit_Inst (Ctx.E, "store.i32 " & To_String (Lasts (D_Idx)) & ", " & LF & ", align 4");
                           end;
                        end loop;
                     end;
                     Emit_Copy_Outs;
                     -- Do NOT release SS here: returned buffer lives on caller's Secondary Stack!
                     Emit_Term (Ctx.E, "ret");
                  end;
               elsif Ctx.Cur_Ret_Comp then
                  declare
                     Ret_T  : constant TREE := Ctx.Cur_Ret_Type;
                     Ret_TI : constant Type_Info := Get_Type_Info (Ctx, Ret_T);
                  begin
                     Emit_Init_Composite (Ctx, To_String (Ctx.Cur_Ret_Ptr), Ret_T, Exp);
                     if Ret_TI.Kind = TK_Record then
                        Emit_Discriminant_Check (Ctx, To_String (Ctx.Cur_Ret_Ptr), Ret_T, Is_Access => False);
                     end if;
                  end;
                  Emit_Copy_Outs;
                  Emit_SS_Release_To (Ctx, 0);
                  Emit_Term (Ctx.E, "ret");
               else
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, Ctx.Cur_Ret_Type);
                     V   : constant Expr_Val := Emit_Expr (Ctx, Exp, Ctx.Cur_Ret_Type);
                     SP  : constant String := To_String (V.Pir_Type);
                     DP  : constant String := To_String (RTI.Pir_Type);
                     VR  : Unbounded_String;
                  begin
                     if Int_Bits_Of_Pir (SP) > Int_Bits_Of_Pir (DP) and then Is_Discrete_Type (Ctx.Cur_Ret_Type) then
                        Emit_Range_Check (Ctx, To_String (V.Reg), SP, Ctx.Cur_Ret_Type);
                     end if;
                     VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (V.Reg), SP, DP));
                     Emit_Range_Check (Ctx, To_String (VR), DP, Ctx.Cur_Ret_Type);
                     Emit_Discriminant_Check (Ctx, To_String (VR), Ctx.Cur_Ret_Type, Is_Access => True);
                     Emit_Copy_Outs;
                     Emit_SS_Release_To (Ctx, 0);
                     Emit_Term (Ctx.E, "ret " & To_String (VR));
                  end;
               end if;
            end;

         when DN_RAISE =>
            declare
               EN : TREE := D (AS_NAME, Stm);
            begin
               if Is_Void_Tree (EN) then
                  -- Bare "raise;" inside exception handler: re-raise current caught exception
                  for I in reverse 0 .. Natural (Ctx.Handlers.Length) - 1 loop
                     if Ctx.Handlers.Element (I).In_Handler then
                        Emit_Raise_Reg (Ctx, To_String (Ctx.Handlers.Element (I).Current_Exn));
                        return;
                     end if;
                  end loop;
                  Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
               else
                  if KIND (EN) = DN_SELECTED then
                     EN := D (AS_DESIGNATOR, EN);
                  end if;
                  Emit_Raise_Tag (Ctx, Get_Exc_Tag (Ctx, D (SM_DEFN, EN)));
               end if;
            end;

         when DN_DELAY =>
            declare
               Ns    : constant String := Emit_Duration_Ns (Ctx, D (AS_EXP, Stm));
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_delay", Ns, "", Dummy, Nounwind => True);
            end;

         when DN_ABORT =>
            -- LRM 9.10(6): evaluate ALL task names first, then abort all (aborting Self last)
            declare
               NS : constant TREE := D (AS_NAME_S, Stm);
            begin
               if not Is_Void_Tree (NS) then
                  declare
                     NL   : SEQ_TYPE := LIST (NS);
                     TN   : TREE;
                     Regs : Str_Vecs.Vector;
                  begin
                     while not IS_EMPTY (NL) loop
                        POP (NL, TN);
                        declare
                           TV : constant Expr_Val := Emit_Expr (Ctx, TN);
                        begin
                           Regs.Append (TV.Reg);
                        end;
                     end loop;
                     if Natural (Regs.Length) = 1 then
                        declare
                           Dummy : Unbounded_String;
                        begin
                           Emit_Call_Or_Invoke (Ctx, "@__ada_task_abort", To_String (Regs.Element (0)), "", Dummy, Nounwind => True);
                        end;
                     elsif not Regs.Is_Empty then
                        declare
                           Cnt   : constant Natural := Natural (Regs.Length);
                           Arr   : constant String := Next_Reg (Ctx.E, "abarr");
                           Dummy : Unbounded_String;
                        begin
                           Emit_Entry_Alloca (Ctx.E, Arr & " = alloca [" & Img (Cnt) & " x ptr], align 8");
                           for I in 0 .. Cnt - 1 loop
                              declare
                                 EP : constant String := Next_Reg (Ctx.E, "abep");
                              begin
                                 Emit_Inst (Ctx.E, EP & " = elem_ptr ptr, " & Arr & ", " & Img (I));
                                 Emit_Inst (Ctx.E, "store.ptr " & To_String (Regs.Element (I)) & ", " & EP & ", align 8");
                              end;
                           end loop;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_task_abort_list", Arr & ", " & Img (Cnt), "", Dummy, Nounwind => True);
                        end;
                     end if;
                  end;
               end if;
            end;

         when DN_TERMINATE =>
            if Length (Ctx.Cur_Task_Exit_Lbl) > 0 then
               -- Leave any nested block masters inside the task body (task's own master at index 0 is left at Cur_Task_Exit_Lbl)
               declare
                  Keep : constant Natural := (if Natural (Ctx.Master_Stack.Length) > 0 and then Ctx.Master_Stack.Element (0) /= Null_Unbounded_String then 1 else 0);
               begin
                  if Natural (Ctx.Master_Stack.Length) > Keep then
                     Emit_Master_Leave_To (Ctx, Keep);
                  end if;
               end;
               Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));
            else
               Emit_Term (Ctx.E, "ret");
            end if;

         when DN_PROCEDURE_CALL | DN_ENTRY_CALL | DN_COND_ENTRY | DN_TIMED_ENTRY =>
            declare
               -- P3-4.11: Resolve cross-unit & chained renamed task entries (for normal, conditional, and timed entry calls!)
               function Try_Bind_Cross_Unit_Renamed_Entry (PDef : TREE) return Boolean is
               begin
                  if Is_Void_Tree (PDef) then
                     return False;
                  end if;
                  if Ctx.Renamed_Entries.Contains (Tree_Key (PDef)) then
                     return True;
                  end if;
                  if KIND (PDef) = DN_PROCEDURE_ID
                    and then not Is_Void_Tree (D (SM_UNIT_DESC, PDef))
                    and then KIND (D (SM_UNIT_DESC, PDef)) = DN_RENAMES_UNIT
                  then
                     declare
                        Cur_P    : TREE := PDef;
                        Leaf_Ren : TREE := PDef;
                        Tgt_E    : TREE := CONST_VOID;
                        Tsk_T    : TREE := CONST_VOID;
                     begin
                        for Step in 1 .. 8 loop
                           exit when Is_Void_Tree (Cur_P);
                           if KIND (Cur_P) = DN_ENTRY_ID then
                              Tgt_E := Cur_P;
                              exit;
                           elsif KIND (Cur_P) = DN_PROCEDURE_ID
                             and then not Is_Void_Tree (D (SM_UNIT_DESC, Cur_P))
                             and then KIND (D (SM_UNIT_DESC, Cur_P)) = DN_RENAMES_UNIT
                           then
                              Leaf_Ren := Cur_P;
                              declare
                                 RN : TREE := D (AS_NAME, D (SM_UNIT_DESC, Cur_P));
                              begin
                                 if not Is_Void_Tree (RN) and then KIND (RN) = DN_INDEXED then
                                    RN := D (AS_NAME, RN);
                                 end if;
                                 if not Is_Void_Tree (RN) and then KIND (RN) = DN_SELECTED then
                                    Cur_P := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, RN))));
                                    if not Is_Void_Tree (Cur_P) and then KIND (Cur_P) = DN_ENTRY_ID
                                      and then not Is_Void_Tree (D (AS_NAME, RN))
                                      and then KIND (D (AS_NAME, RN)) in CLASS_EXP
                                      and then KIND (D (AS_NAME, RN)) /= DN_USED_NAME_ID
                                    then
                                       Tsk_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, D (AS_NAME, RN))));
                                    end if;
                                 elsif not Is_Void_Tree (RN) then
                                    Cur_P := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (RN)));
                                 else
                                    exit;
                                 end if;
                              end;
                           else
                              exit;
                           end if;
                        end loop;
                        if not Is_Void_Tree (Tgt_E) then
                           if not Is_Void_Tree (Tsk_T) then
                              if KIND (Tsk_T) = DN_ACCESS then
                                 Register_Task_Spec (Ctx, D (SM_DESIG_TYPE, Tsk_T), Ctx.Cur_Subp_Key);
                              else
                                 Register_Task_Spec (Ctx, Tsk_T, Ctx.Cur_Subp_Key);
                              end if;
                           elsif KIND (Tgt_E) in CLASS_SOURCE_NAME and then not Is_Void_Tree (D (XD_REGION, Tgt_E)) then
                              declare
                                 TR : constant TREE := D (XD_REGION, Tgt_E);
                              begin
                                 if KIND (TR) in CLASS_TYPE_NAME then
                                    Register_Task_Spec (Ctx, D (SM_TYPE_SPEC, TR), Ctx.Cur_Subp_Key);
                                 elsif KIND (TR) = DN_VARIABLE_ID then
                                    Register_Task_Spec (Ctx, D (SM_OBJ_TYPE, TR), Ctx.Cur_Subp_Key);
                                 end if;
                              end;
                           end if;
                           declare
                              RQName : constant String := Qualified_Ada_Name (Leaf_Ren);
                              TSym   : constant String := "@ada_rent_t__" & RQName;
                              ISym   : constant String := "@ada_rent_i__" & RQName;
                           begin
                              Add_Global_Decl (Ctx.E, TSym, "extern " & TSym & " = global ptr, align 8");
                              Add_Global_Decl (Ctx.E, ISym, "extern " & ISym & " = global i32, align 4");
                              Ctx.Renamed_Entries.Include (Tree_Key (PDef),
                                (Task_Sym      => To_Unbounded_String (TSym),
                                 Idx_Sym       => To_Unbounded_String (ISym),
                                 Entry_Id      => Tgt_E,
                                 Is_Local      => False,
                                 Owner_Subp    => 0,
                                 Fallback_TSym => Null_Unbounded_String,
                                 Fallback_ISym => Null_Unbounded_String));
                              return True;
                           end;
                        end if;
                     end;
                  end if;
                  return False;
               end Try_Bind_Cross_Unit_Renamed_Entry;
            begin
               if K = DN_PROCEDURE_CALL then
                  declare
                     PDef : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, Stm))));
                  begin
                     if not Try_Bind_Cross_Unit_Renamed_Entry (PDef) then
                        declare
                           Dummy : constant Expr_Val := Emit_Subp_Call (Ctx, Stm, Is_Func => False);
                        begin
                           pragma Unreferenced (Dummy);
                        end;
                        return;
                     end if;
                  end;
               end if;
            declare
               type Entry_Copy_Back is record
                  Field_Idx : Natural := 0;
                  Act_Ptr   : Unbounded_String;
                  Act_Pir   : Unbounded_String;
                  Act_Align : Natural := 4;
                  Act_Type  : TREE := CONST_VOID;
                  Conv_Type : TREE := CONST_VOID;
                  Pkt_Pir   : Unbounded_String;
                  Pkt_Align : Natural := 4;
                  Pkt_Type  : TREE := CONST_VOID;
                  Is_Shared : Boolean := False;
               end record;
               package ECB_Vecs is new Ada.Containers.Vectors (Natural, Entry_Copy_Back);

               -- Helper: emit entry call (Mode: 0=normal, 1=cond, 2=timed)
               procedure Emit_Entry_Call_Core
                 (EC_Node   : TREE;
                  Mode      : Natural;
                  Delay_Exp : TREE := CONST_VOID;
                  Taken_Out : out Unbounded_String;
                  CB_List   : out ECB_Vecs.Vector;
                  Out_Pkt   : out Unbounded_String;
                  Out_PktT  : out Unbounded_String)
               is
                  Name_N   : TREE := D (AS_NAME, EC_Node);
                  Fam_Exp  : TREE := CONST_VOID;
                  Task_Exp : TREE := CONST_VOID;
                  Entry_Id : TREE := CONST_VOID;
                  Task_Ptr : Unbounded_String;
                  EIdx_Reg : Unbounded_String := To_Unbounded_String ("0");
                  EI       : Entry_Info;
                  Has_EI   : Boolean := False;
                  Pkt_Reg  : Unbounded_String := To_Unbounded_String ("0");
                  Norm_S   : constant TREE := D (SM_NORMALIZED_PARAM_S, EC_Node);
                  Param_L  : SEQ_TYPE := MAKE;
                  Delay_Ns : Unbounded_String := To_Unbounded_String ("0");
               begin
                  Taken_Out := Null_Unbounded_String;
                  CB_List.Clear;
                  Out_Pkt   := Null_Unbounded_String;
                  Out_PktT  := Null_Unbounded_String;
                  if not Is_Void_Tree (Norm_S) then
                     Param_L := LIST (Norm_S);
                  elsif not Is_Void_Tree (D (AS_GENERAL_ASSOC_S, EC_Node)) then
                     Param_L := LIST (D (AS_GENERAL_ASSOC_S, EC_Node));
                  end if;
                  declare
                     Call_Def : constant TREE :=
                       (if KIND (Name_N) /= DN_INDEXED
                        then Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Name_N))) else CONST_VOID);
                     Is_Ren_Ent : constant Boolean := Try_Bind_Cross_Unit_Renamed_Entry (Call_Def);
                  begin
                  if Is_Ren_Ent then
                     declare
                        REI : constant Renamed_Entry_Info := Ctx.Renamed_Entries.Element (Tree_Key (Call_Def));
                        TV  : constant String := Next_Reg (Ctx.E, "rtpv");
                        IV  : constant String := Next_Reg (Ctx.E, "reiv");
                     begin
                        if REI.Is_Local then
                           declare
                              Base_P   : Unbounded_String := REI.Task_Sym;
                              Found_SL : Boolean := (REI.Owner_Subp = Ctx.Cur_Subp_Key);
                              Off_I    : constant String := Next_Reg (Ctx.E, "reio");
                           begin
                              if not Found_SL and then Ctx.Vars.Contains (Tree_Key (Call_Def))
                                and then Ctx.Vars.Element (Tree_Key (Call_Def)).Captured_Slot > 0
                              then
                                 declare
                                    Cap_Sl : constant Natural := Ctx.Vars.Element (Tree_Key (Call_Def)).Captured_Slot;
                                    Cur_SK : Unsigned_32 := Ctx.Cur_Subp_Key;
                                    Cur_SL : Unbounded_String := Ctx.Cur_SL_Reg;
                                 begin
                                    for Step in 1 .. 16 loop
                                       exit when Cur_SK = 0 or else not Ctx.Subps.Contains (Cur_SK);
                                       declare
                                          PSI : constant Subp_Info := Ctx.Subps.Element (Cur_SK);
                                          Par : constant Unsigned_32 := PSI.Parent_Subp;
                                       begin
                                          exit when Par = 0 or else not Ctx.Subps.Contains (Par);
                                          declare
                                             OSI : constant Subp_Info := Ctx.Subps.Element (Par);
                                          begin
                                             if Par = REI.Owner_Subp then
                                                declare
                                                   FP : constant String := Next_Reg (Ctx.E, "rslp");
                                                   LP : constant String := Next_Reg (Ctx.E, "rslv");
                                                begin
                                                   Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (OSI.Frame_Type) & ", " & To_String (Cur_SL) & ", " & Img (Cap_Sl));
                                                   Emit_Inst (Ctx.E, LP & " = load.ptr " & FP & ", align 8");
                                                   Base_P := To_Unbounded_String (LP);
                                                   Found_SL := True;
                                                end;
                                                exit;
                                             else
                                                declare
                                                   FP : constant String := Next_Reg (Ctx.E, "rsln");
                                                   NP : constant String := Next_Reg (Ctx.E, "rslq");
                                                begin
                                                   Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (OSI.Frame_Type) & ", " & To_String (Cur_SL) & ", 0");
                                                   Emit_Inst (Ctx.E, NP & " = load.ptr " & FP & ", align 8");
                                                   Cur_SL := To_Unbounded_String (NP);
                                                   Cur_SK := Par;
                                                end;
                                             end if;
                                          end;
                                       end;
                                    end loop;
                                 end;
                              end if;
                              if Found_SL then
                                 Emit_Inst (Ctx.E, TV & " = load.ptr " & To_String (Base_P) & ", align 8");
                                 declare
                                    Off8 : constant String := Next_Reg (Ctx.E, "rei8");
                                 begin
                                    Emit_Inst (Ctx.E, Off8 & " = const.i64 8");
                                    Emit_Inst (Ctx.E, Off_I & " = ptr_add.i64 " & To_String (Base_P) & ", " & Off8);
                                 end;
                                 Emit_Inst (Ctx.E, IV & " = load.i32 " & Off_I & ", align 4");
                              else
                                 declare
                                    TA : constant String := Next_Reg (Ctx.E, "rtpa");
                                    IA : constant String := Next_Reg (Ctx.E, "reia");
                                 begin
                                    Emit_Inst (Ctx.E, TA & " = addr " & To_String (REI.Fallback_TSym));
                                    Emit_Inst (Ctx.E, TV & " = load.ptr " & TA & ", align 8");
                                    Emit_Inst (Ctx.E, IA & " = addr " & To_String (REI.Fallback_ISym));
                                    Emit_Inst (Ctx.E, IV & " = load.i32 " & IA & ", align 4");
                                 end;
                              end if;
                           end;
                        else
                           declare
                              TA : constant String := Next_Reg (Ctx.E, "rtpa");
                              IA : constant String := Next_Reg (Ctx.E, "reia");
                           begin
                              Emit_Inst (Ctx.E, TA & " = addr " & To_String (REI.Task_Sym));
                              Emit_Inst (Ctx.E, TV & " = load.ptr " & TA & ", align 8");
                              Emit_Inst (Ctx.E, IA & " = addr " & To_String (REI.Idx_Sym));
                              Emit_Inst (Ctx.E, IV & " = load.i32 " & IA & ", align 4");
                           end;
                        end if;
                        Task_Ptr := To_Unbounded_String (TV);
                        EIdx_Reg := To_Unbounded_String (IV);
                        Entry_Id := REI.Entry_Id;
                        if not Is_Void_Tree (Entry_Id) and then Ctx.Entries.Contains (Tree_Key (Entry_Id)) then
                           EI := Ctx.Entries.Element (Tree_Key (Entry_Id));
                           Has_EI := True;
                        end if;
                     end;
                  else
                  if KIND (Name_N) = DN_INDEXED then
                     declare
                        EL : SEQ_TYPE := LIST (D (AS_EXP_S, Name_N));
                     begin
                        if not IS_EMPTY (EL) then POP (EL, Fam_Exp); end if;
                     end;
                     Name_N := D (AS_NAME, Name_N);
                  end if;
                  if KIND (Name_N) = DN_SELECTED then
                     Task_Exp := D (AS_NAME, Name_N);
                     Entry_Id := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, Name_N))));
                  else
                     Entry_Id := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Name_N)));
                  end if;
                  if not Is_Void_Tree (Entry_Id) and then Ctx.Entries.Contains (Tree_Key (Entry_Id)) then
                     EI := Ctx.Entries.Element (Tree_Key (Entry_Id));
                     Has_EI := True;
                  end if;
                  if not Is_Void_Tree (Task_Exp) then
                     declare
                        TV : constant Expr_Val := Emit_Expr (Ctx, Task_Exp);
                        TT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, TV.Ada_Type));
                     begin
                        if Is_Access_Type (TT) then
                           Emit_Null_Check (Ctx, To_String (TV.Reg));
                           declare
                              LP : constant String := Next_Reg (Ctx.E, "atp");
                           begin
                              Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (TV.Reg) & ", align 8");
                              Task_Ptr := To_Unbounded_String (LP);
                           end;
                           if KIND (TT) = DN_ACCESS then
                              Register_Task_Spec (Ctx, D (SM_DESIG_TYPE, TT), Ctx.Cur_Subp_Key);
                           end if;
                        else
                           Task_Ptr := TV.Reg;
                           Register_Task_Spec (Ctx, TT, Ctx.Cur_Subp_Key);
                        end if;
                     end;
                  else
                     Task_Ptr := To_Unbounded_String (Emit_Enclosing_Task_Self_By_Key (Ctx, (if Has_EI then EI.Task_Key else 0)));
                  end if;

                  if not Is_Void_Tree (Entry_Id) and then Ctx.Entries.Contains (Tree_Key (Entry_Id)) then
                     EI := Ctx.Entries.Element (Tree_Key (Entry_Id));
                     Has_EI := True;
                  end if;

                  if Has_EI then
                     EIdx_Reg := To_Unbounded_String (Emit_Entry_Index_Reg (Ctx, EI, Fam_Exp));
                  end if;
                  end if;
                  end;

                  if Has_EI then

                     -- Marshal parameters into stack packet struct (evaluating actuals BEFORE delay!)
                     if EI.Has_Params and then not Is_Void_Tree (EI.Entry_Header)
                       and then not Is_Void_Tree (D (AS_PARAM_S, EI.Entry_Header))
                     then
                        declare
                           PR   : constant String := Next_Reg (Ctx.E, "epkt");
                           FPL  : SEQ_TYPE := LIST (D (AS_PARAM_S, EI.Entry_Header));
                           FP_N : TREE;
                           FIdx : Natural := 0;
                        begin
                           Emit_Entry_Alloca (Ctx.E, PR & " = alloca " & To_String (EI.Pkt_Type) & ", align 8");
                           Pkt_Reg  := To_Unbounded_String (PR);
                           Out_Pkt  := Pkt_Reg;
                           Out_PktT := EI.Pkt_Type;
                           while not IS_EMPTY (FPL) loop
                              POP (FPL, FP_N);
                              declare
                                 PMode : constant NODE_NAME := KIND (FP_N);
                                 NS    : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FP_N));
                                 PID   : TREE;
                              begin
                                 while not IS_EMPTY (NS) loop
                                    POP (NS, PID);
                                    declare
                                       Act : TREE := CONST_VOID;
                                       PT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                                       PTI : constant Type_Info := Get_Type_Info (Ctx, PT);
                                       Fld : constant String := Next_Reg (Ctx.E, "epf");
                                    begin
                                       if not IS_EMPTY (Param_L) then
                                          POP (Param_L, Act);
                                          if KIND (Act) = DN_ASSOC or else KIND (Act) = DN_NAMED then
                                             Act := D (AS_EXP, Act);
                                          end if;
                                       else
                                          Act := D (SM_INIT_EXP, PID);
                                       end if;
                                       if PTI.Kind = TK_Unconstrained_Array then
                                          declare
                                             AV : constant Expr_Val := (if PMode = DN_IN then Emit_Expr (Ctx, Act, PT) else Emit_LValue (Ctx, Act));
                                             ND : constant Natural := Natural'Max (1, PTI.NDims);
                                          begin
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                             Emit_Inst (Ctx.E, "store.ptr " & To_String (AV.Reg) & ", " & Fld & ", align 8");
                                             FIdx := FIdx + 1;
                                             if Needs_Constrained_Param (Ctx, PT, PMode) then
                                                declare
                                                   CFld : constant String := Next_Reg (Ctx.E, "ecpf");
                                                   CVal : constant String := Emit_Actual_Constrained (Ctx, Act);
                                                begin
                                                   Emit_Inst (Ctx.E, CFld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, "store.i8 " & CVal & ", " & CFld & ", align 1");
                                                   FIdx := FIdx + 1;
                                                end;
                                             end if;
                                             for D_I in 1 .. ND loop
                                                declare
                                                   FR, LR : Unbounded_String;
                                                   FF : constant String := Next_Reg (Ctx.E, "epff");
                                                   LF : constant String := Next_Reg (Ctx.E, "eplf");
                                                begin
                                                   Get_Array_Dim_Bounds (Ctx, AV, D_I, FR, LR);
                                                   Emit_Inst (Ctx.E, FF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, "store.i32 " & To_String (FR) & ", " & FF & ", align 4");
                                                   FIdx := FIdx + 1;
                                                   Emit_Inst (Ctx.E, LF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, "store.i32 " & To_String (LR) & ", " & LF & ", align 4");
                                                   FIdx := FIdx + 1;
                                                end;
                                             end loop;
                                          end;
                                       elsif PTI.Kind /= TK_Scalar then
                                          declare
                                             AV : constant Expr_Val := (if PMode = DN_IN then Emit_Expr (Ctx, Act, PT) else Emit_LValue (Ctx, Act));
                                          begin
                                             if PTI.Kind = TK_Constrained_Array then
                                                declare
                                                   Tgt_V : Expr_Val;
                                                   ND_A  : constant Natural := Natural'Max (1, Natural'Min (PTI.NDims, Max_Array_Dims));
                                                   Act_U : TREE := Act;
                                                begin
                                                   while not Is_Void_Tree (Act_U) and then KIND (Act_U) = DN_PARENTHESIZED loop
                                                      Act_U := D (AS_EXP, Act_U);
                                                   end loop;
                                                   Tgt_V.Kind := EK_Slice; Tgt_V.Ada_Type := PT; Tgt_V.NDims := ND_A;
                                                   for D_I in 1 .. ND_A loop
                                                      Emit_Index_Bounds (Ctx, PTI.Dims (D_I).Idx_Type, Tgt_V.First_Regs (D_I), Tgt_V.Last_Regs (D_I));
                                                   end loop;
                                                   if not Is_Void_Tree (Act_U) and then KIND (Act_U) in DN_STRING_LITERAL | DN_AGGREGATE then
                                                      Emit_Array_Length_Check (Ctx, Tgt_V, AV, ND_A);
                                                   elsif Ctx.Enable_Checks then
                                                      -- Ada 83 LRM 6.4.1(5) (c95085c, c85018b): exact bounds match for all constrained array entry params
                                                      for D_I in 1 .. ND_A loop
                                                         declare
                                                            AF, AL : Unbounded_String;
                                                            C1  : constant String := Next_Reg (Ctx.E, "ebf1");
                                                            C2  : constant String := Next_Reg (Ctx.E, "ebf2");
                                                            Bad : constant String := Next_Reg (Ctx.E, "ebfb");
                                                         begin
                                                            Get_Array_Dim_Bounds (Ctx, AV, D_I, AF, AL);
                                                            Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (AF) & ", " & To_String (Tgt_V.First_Regs (D_I)));
                                                            Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (AL) & ", " & To_String (Tgt_V.Last_Regs (D_I)));
                                                            Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                                            Emit_Check_Fail_If (Ctx, Bad);
                                                         end;
                                                      end loop;
                                                   end if;
                                                end;
                                             else
                                                Emit_Discriminant_Check (Ctx, To_String (AV.Reg), PT, Is_Access => False);
                                             end if;
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                             Emit_Inst (Ctx.E, "store.ptr " & To_String (AV.Reg) & ", " & Fld & ", align 8");
                                             FIdx := FIdx + 1;
                                             if Needs_Constrained_Param (Ctx, PT, PMode) then
                                                declare
                                                   CFld : constant String := Next_Reg (Ctx.E, "ecpf");
                                                   CVal : constant String := Emit_Actual_Constrained (Ctx, Act);
                                                begin
                                                   Emit_Inst (Ctx.E, CFld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, "store.i8 " & CVal & ", " & CFld & ", align 1");
                                                   FIdx := FIdx + 1;
                                                end;
                                             end if;
                                          end;
                                       elsif PMode = DN_OUT or else PMode = DN_IN_OUT then
                                          -- Copy-in / copy-out scalar actual (evaluates lvalue ONCE before call, handles DN_CONVERSION!)
                                          declare
                                             Inner_Act : TREE := Act;
                                             Conv_T    : TREE := CONST_VOID;
                                          begin
                                             while not Is_Void_Tree (Inner_Act) and then KIND (Inner_Act) = DN_CONVERSION loop
                                                if Is_Void_Tree (Conv_T) then
                                                   Conv_T := Subst_Tree (Ctx, D (SM_EXP_TYPE, Inner_Act));
                                                end if;
                                                Inner_Act := D (AS_EXP, Inner_Act);
                                             end loop;
                                             declare
                                                LV  : constant Expr_Val := Emit_LValue (Ctx, Inner_Act);
                                                ATI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
                                                Act_Sh : constant Boolean := Is_Shared_Object (Ctx, Inner_Act);
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                                if PMode = DN_IN_OUT then
                                                   declare
                                                      RV : constant String := Next_Reg (Ctx.E, "ecin");
                                                      VR : Unbounded_String;
                                                   begin
                                                      if Act_Sh then
                                                         Emit_Inst (Ctx.E, RV & " = load." & To_String (ATI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (ATI.Align) & " [volatile]");
                                                         Emit_Inst (Ctx.E, "fence [seq_cst]");
                                                      else
                                                         Emit_Inst (Ctx.E, RV & " = load." & To_String (ATI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (ATI.Align));
                                                      end if;
                                                      if not Is_Void_Tree (Conv_T) then
                                                         declare
                                                            CTI : constant Type_Info := Get_Type_Info (Ctx, Conv_T);
                                                            CV  : constant String := Emit_Typed_Scalar_Convert (Ctx, RV, LV.Ada_Type, To_String (ATI.Pir_Type), Conv_T, To_String (CTI.Pir_Type));
                                                         begin
                                                            Emit_Range_Check (Ctx, CV, To_String (CTI.Pir_Type), Conv_T);
                                                            VR := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, CV, Conv_T, To_String (CTI.Pir_Type), PT, To_String (PTI.Pir_Type)));
                                                         end;
                                                      else
                                                         VR := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, RV, LV.Ada_Type, To_String (ATI.Pir_Type), PT, To_String (PTI.Pir_Type)));
                                                      end if;
                                                      Emit_Range_Check (Ctx, To_String (VR), To_String (PTI.Pir_Type), PT);
                                                      Emit_Discriminant_Check (Ctx, To_String (VR), PT, Is_Access => True);
                                                      Emit_Inst (Ctx.E, "store." & To_String (PTI.Pir_Type) & " " & To_String (VR) & ", " & Fld & ", align " & Img (PTI.Align));
                                                   end;
                                                end if;
                                                CB_List.Append ((Field_Idx => FIdx,
                                                                 Act_Ptr   => LV.Reg,
                                                                 Act_Pir   => ATI.Pir_Type,
                                                                 Act_Align => ATI.Align,
                                                                 Act_Type  => LV.Ada_Type,
                                                                 Conv_Type => Conv_T,
                                                                 Pkt_Pir   => PTI.Pir_Type,
                                                                 Pkt_Align => PTI.Align,
                                                                 Pkt_Type  => PT,
                                                                 Is_Shared => Act_Sh));
                                                FIdx := FIdx + 1;
                                             end;
                                          end;
                                       else
                                          declare
                                             AV : constant Expr_Val := Emit_Expr (Ctx, Act, PT);
                                             VR : constant String := Emit_Scalar_Coerce (Ctx, To_String (AV.Reg), To_String (AV.Pir_Type), To_String (PTI.Pir_Type));
                                          begin
                                             Emit_Range_Check (Ctx, VR, To_String (PTI.Pir_Type), PT);
                                             Emit_Discriminant_Check (Ctx, VR, PT, Is_Access => True);
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & PR & ", " & Img (FIdx));
                                             Emit_Inst (Ctx.E, "store." & To_String (PTI.Pir_Type) & " " & VR & ", " & Fld & ", align " & Img (PTI.Align));
                                             FIdx := FIdx + 1;
                                          end;
                                       end if;
                                    end;
                                 end loop;
                              end;
                           end loop;
                        end;
                     end if;
                  end if;

                  -- For timed entry calls, evaluate delay expression AFTER entry name & parameters (LRM 9.7.3)
                  if Mode = 2 and then not Is_Void_Tree (Delay_Exp) then
                     Delay_Ns := To_Unbounded_String (Emit_Duration_Ns (Ctx, Delay_Exp));
                  end if;

                  declare
                     RV : Unbounded_String;
                  begin
                     if Mode = 0 then
                        Emit_Call_Or_Invoke (Ctx, "@__ada_entry_call",
                          To_String (Task_Ptr) & ", " & To_String (EIdx_Reg) & ", " & To_String (Pkt_Reg), "", RV);
                     elsif Mode = 1 then
                        Emit_Call_Or_Invoke (Ctx, "@__ada_entry_call_cond",
                          To_String (Task_Ptr) & ", " & To_String (EIdx_Reg) & ", " & To_String (Pkt_Reg), "i8", RV);
                        Taken_Out := RV;
                     else
                        Emit_Call_Or_Invoke (Ctx, "@__ada_entry_call_timed",
                          To_String (Task_Ptr) & ", " & To_String (EIdx_Reg) & ", " & To_String (Pkt_Reg) & ", " & To_String (Delay_Ns), "i8", RV);
                        Taken_Out := RV;
                     end if;
                  end;
               end Emit_Entry_Call_Core;

               procedure Emit_Entry_Copy_Backs
                 (CB_List : ECB_Vecs.Vector;
                  Pkt_Reg : String;
                  Pkt_Ty  : String)
               is
               begin
                  for CB of CB_List loop
                     declare
                        Fld : constant String := Next_Reg (Ctx.E, "ecbf");
                        PV  : constant String := Next_Reg (Ctx.E, "ecbv");
                        CV  : Unbounded_String;
                     begin
                        Emit_Inst (Ctx.E, Fld & " = field_ptr " & Pkt_Ty & ", " & Pkt_Reg & ", " & Img (CB.Field_Idx));
                        Emit_Inst (Ctx.E, PV & " = load." & To_String (CB.Pkt_Pir) & " " & Fld & ", align " & Img (CB.Pkt_Align));
                        if not Is_Void_Tree (CB.Conv_Type) then
                           declare
                              CTI : constant Type_Info := Get_Type_Info (Ctx, CB.Conv_Type);
                              T1  : constant String := Emit_Typed_Scalar_Convert (Ctx, PV, CB.Pkt_Type, To_String (CB.Pkt_Pir), CB.Conv_Type, To_String (CTI.Pir_Type));
                           begin
                              Emit_Range_Check (Ctx, T1, To_String (CTI.Pir_Type), CB.Conv_Type);
                              CV := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, T1, CB.Conv_Type, To_String (CTI.Pir_Type), CB.Act_Type, To_String (CB.Act_Pir)));
                           end;
                        else
                           CV := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, PV, CB.Pkt_Type, To_String (CB.Pkt_Pir), CB.Act_Type, To_String (CB.Act_Pir)));
                        end if;
                        Emit_Range_Check (Ctx, To_String (CV), To_String (CB.Act_Pir), CB.Act_Type);
                        Emit_Discriminant_Check (Ctx, To_String (CV), CB.Act_Type, Is_Access => True);
                        if CB.Is_Shared then
                           Emit_Inst (Ctx.E, "fence [seq_cst]");
                           Emit_Inst (Ctx.E, "store." & To_String (CB.Act_Pir) & " " & To_String (CV) & ", " & To_String (CB.Act_Ptr) & ", align " & Img (CB.Act_Align) & " [volatile]");
                           Emit_Inst (Ctx.E, "fence [seq_cst]");
                        else
                           Emit_Inst (Ctx.E, "store." & To_String (CB.Act_Pir) & " " & To_String (CV) & ", " & To_String (CB.Act_Ptr) & ", align " & Img (CB.Act_Align));
                        end if;
                     end;
                  end loop;
               end Emit_Entry_Copy_Backs;
            begin
               if K = DN_ENTRY_CALL or else K = DN_PROCEDURE_CALL then
                  declare
                     Taken : Unbounded_String;
                     CBL   : ECB_Vecs.Vector;
                     PR, PT : Unbounded_String;
                  begin
                     Emit_Entry_Call_Core (Stm, 0, CONST_VOID, Taken, CBL, PR, PT);
                     Emit_Entry_Copy_Backs (CBL, To_String (PR), To_String (PT));
                  end;
               elsif K = DN_COND_ENTRY then
                  declare
                     L1     : SEQ_TYPE := LIST (D (AS_STM_S1, Stm));
                     EC_S   : TREE := CONST_VOID;
                     L_Tkn  : constant String := Next_Label (Ctx.E, "ce_t");
                     L_Els  : constant String := Next_Label (Ctx.E, "ce_e");
                     L_End  : constant String := Next_Label (Ctx.E, "ce_end");
                     Taken  : Unbounded_String;
                     CBL    : ECB_Vecs.Vector;
                     PR, PT : Unbounded_String;
                  begin
                     while not IS_EMPTY (L1) loop
                        POP (L1, EC_S);
                        exit when KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL;
                     end loop;
                     if not Is_Void_Tree (EC_S) and then (KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL) then
                        Emit_Entry_Call_Core (EC_S, 1, CONST_VOID, Taken, CBL, PR, PT);
                        Emit_Term (Ctx.E, "br " & To_String (Taken) & ", " & L_Tkn & ", " & L_Els);
                        Emit_Label (Ctx.E, L_Tkn);
                        Emit_Entry_Copy_Backs (CBL, To_String (PR), To_String (PT));
                        while not IS_EMPTY (L1) loop
                           declare
                              S2 : TREE;
                           begin
                              POP (L1, S2);
                              if not Is_Terminated (Ctx.E) or else KIND (S2) = DN_LABELED then
                                 Emit_Stm (Ctx, S2);
                              end if;
                           end;
                        end loop;
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_Els);
                        Emit_Stm_List (Ctx, D (AS_STM_S2, Stm));
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_End);
                     end if;
                  end;
               else -- DN_TIMED_ENTRY
                  declare
                     L1     : SEQ_TYPE := LIST (D (AS_STM_S1, Stm));
                     L2     : SEQ_TYPE := LIST (D (AS_STM_S2, Stm));
                     EC_S   : TREE := CONST_VOID;
                     DL_S   : TREE := CONST_VOID;
                     D_Exp  : TREE := CONST_VOID;
                     L_Tkn  : constant String := Next_Label (Ctx.E, "te_t");
                     L_Del  : constant String := Next_Label (Ctx.E, "te_d");
                     L_End  : constant String := Next_Label (Ctx.E, "te_end");
                     Taken  : Unbounded_String;
                     CBL    : ECB_Vecs.Vector;
                     PR, PT : Unbounded_String;
                  begin
                     while not IS_EMPTY (L1) loop
                        POP (L1, EC_S);
                        exit when KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL;
                     end loop;
                     while not IS_EMPTY (L2) loop
                        POP (L2, DL_S);
                        exit when KIND (DL_S) = DN_DELAY;
                     end loop;
                     if not Is_Void_Tree (DL_S) and then KIND (DL_S) = DN_DELAY then
                        D_Exp := D (AS_EXP, DL_S);
                     end if;
                     if not Is_Void_Tree (EC_S) and then (KIND (EC_S) = DN_ENTRY_CALL or else KIND (EC_S) = DN_PROCEDURE_CALL) then
                        Emit_Entry_Call_Core (EC_S, 2, D_Exp, Taken, CBL, PR, PT);
                        Emit_Term (Ctx.E, "br " & To_String (Taken) & ", " & L_Tkn & ", " & L_Del);
                        Emit_Label (Ctx.E, L_Tkn);
                        Emit_Entry_Copy_Backs (CBL, To_String (PR), To_String (PT));
                        while not IS_EMPTY (L1) loop
                           declare
                              S2 : TREE;
                           begin
                              POP (L1, S2);
                              if not Is_Terminated (Ctx.E) or else KIND (S2) = DN_LABELED then
                                 Emit_Stm (Ctx, S2);
                              end if;
                           end;
                        end loop;
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_Del);
                        while not IS_EMPTY (L2) loop
                           declare
                              S2 : TREE;
                           begin
                              POP (L2, S2);
                              if not Is_Terminated (Ctx.E) or else KIND (S2) = DN_LABELED then
                                 Emit_Stm (Ctx, S2);
                              end if;
                           end;
                        end loop;
                        if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        Emit_Label (Ctx.E, L_End);
                     end if;
                  end;
               end if;
            end;
            end;

         when DN_ACCEPT | DN_SELECTIVE_WAIT =>
            declare
               procedure Emit_Accept_Core
                 (Acc_Node : TREE;
                  Self_Reg : String;
                  Pkt_Ptr  : String;
                  Call_Reg : String)
               is
                  E_Name : TREE := D (AS_NAME, Acc_Node);
                  E_Def  : TREE;
                  Bound_Ks : U32_Vecs.Vector;
                  Dummy  : Unbounded_String;
                  Acc_PS : constant TREE := D (AS_PARAM_S, Acc_Node);
                  Acc_SS : constant TREE := D (AS_STM_S, Acc_Node);
                  Has_Body : constant Boolean := not Is_Void_Tree (Acc_SS) and then not IS_EMPTY (LIST (Acc_SS));
               begin
                  if KIND (E_Name) = DN_INDEXED then
                     E_Name := D (AS_NAME, E_Name);
                  end if;
                  E_Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (E_Name)));
                  if Has_Body and then not Is_Void_Tree (E_Def) and then Ctx.Entries.Contains (Tree_Key (E_Def)) then
                     declare
                        EI : constant Entry_Info := Ctx.Entries.Element (Tree_Key (E_Def));
                        FIdx : Natural := 0;
                     begin
                        if EI.Has_Params and then not Is_Void_Tree (Acc_PS) then
                           declare
                              PL : SEQ_TYPE := LIST (Acc_PS);
                              P  : TREE;
                           begin
                              while not IS_EMPTY (PL) loop
                                 POP (PL, P);
                                 declare
                                    PMode : constant NODE_NAME := KIND (P);
                                    NS    : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                                    PID   : TREE;
                                 begin
                                    while not IS_EMPTY (NS) loop
                                       POP (NS, PID);
                                       declare
                                          Can_P : constant TREE := Canonical_Def (PID);
                                          PK    : constant Unsigned_32 := Tree_Key (Can_P);
                                          PT    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                                          PTI   : constant Type_Info := Get_Type_Info (Ctx, PT);
                                          Fld   : constant String := Next_Reg (Ctx.E, "apf");
                                          VI    : Var_Info;
                                       begin
                                          VI.Ada_Type   := PT;
                                          VI.Pir_Type   := PTI.Pir_Type;
                                          VI.Align      := PTI.Align;
                                          VI.Owner_Subp := Ctx.Cur_Subp_Key;
                                          if PTI.Kind = TK_Unconstrained_Array then
                                             declare
                                                DP : constant String := Next_Reg (Ctx.E, "apd");
                                                ND : constant Natural := Natural'Max (1, PTI.NDims);
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                Emit_Inst (Ctx.E, DP & " = load.ptr " & Fld & ", align 8");
                                                FIdx := FIdx + 1;
                                                VI.Kind     := VK_Uncon_Array_Param;
                                                VI.Pir_Name := To_Unbounded_String (DP);
                                                VI.NDims    := ND;
                                                for D_I in 1 .. ND loop
                                                   declare
                                                      FF : constant String := Next_Reg (Ctx.E, "apff");
                                                      FV : constant String := Next_Reg (Ctx.E, "apfv");
                                                      LF : constant String := Next_Reg (Ctx.E, "aplf");
                                                      LV : constant String := Next_Reg (Ctx.E, "aplv");
                                                   begin
                                                      Emit_Inst (Ctx.E, FF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                      Emit_Inst (Ctx.E, FV & " = load.i32 " & FF & ", align 4");
                                                      FIdx := FIdx + 1;
                                                      Emit_Inst (Ctx.E, LF & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                      Emit_Inst (Ctx.E, LV & " = load.i32 " & LF & ", align 4");
                                                      FIdx := FIdx + 1;
                                                      VI.First_Regs (D_I) := To_Unbounded_String (FV);
                                                      VI.Last_Regs (D_I)  := To_Unbounded_String (LV);
                                                   end;
                                                end loop;
                                             end;
                                          elsif PTI.Kind /= TK_Scalar then
                                             declare
                                                PV : constant String := Next_Reg (Ctx.E, "app");
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                Emit_Inst (Ctx.E, PV & " = load.ptr " & Fld & ", align 8");
                                                FIdx := FIdx + 1;
                                                VI.Kind     := VK_Param_Ptr;
                                                VI.Pir_Name := To_Unbounded_String (PV);
                                                if Needs_Constrained_Param (Ctx, PT, PMode) then
                                                   declare
                                                      CFld : constant String := Next_Reg (Ctx.E, "apcf");
                                                      CVal : constant String := Next_Reg (Ctx.E, "apcv");
                                                   begin
                                                      Emit_Inst (Ctx.E, CFld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                      Emit_Inst (Ctx.E, CVal & " = load.i8 " & CFld & ", align 1");
                                                      FIdx := FIdx + 1;
                                                      VI.Constrained_Reg := To_Unbounded_String (CVal);
                                                   end;
                                                end if;
                                             end;
                                          elsif PMode = DN_OUT or else PMode = DN_IN_OUT then
                                             -- Copy-in/copy-out scalar param: formal points directly into packet slot!
                                             Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                             FIdx := FIdx + 1;
                                             VI.Kind     := VK_Param_Ptr;
                                             VI.Pir_Name := To_Unbounded_String (Fld);
                                          else
                                             declare
                                                SV : constant String := Next_Reg (Ctx.E, "apv");
                                             begin
                                                Emit_Inst (Ctx.E, Fld & " = field_ptr " & To_String (EI.Pkt_Type) & ", " & Pkt_Ptr & ", " & Img (FIdx));
                                                Emit_Inst (Ctx.E, SV & " = load." & To_String (PTI.Pir_Type) & " " & Fld & ", align " & Img (PTI.Align));
                                                FIdx := FIdx + 1;
                                                VI.Kind     := VK_Param_Val;
                                                VI.Pir_Name := To_Unbounded_String (SV);
                                             end;
                                          end if;
                                          if Ctx.Vars.Contains (PK) and then Ctx.Vars.Element (PK).Owner_Subp = Ctx.Cur_Subp_Key then
                                             VI.Captured_Slot := Ctx.Vars.Element (PK).Captured_Slot;
                                          end if;
                                          if VI.Captured_Slot > 0 and then Length (Ctx.Cur_Frame_Reg) > 0 then
                                             declare
                                                FSI : constant Subp_Info := Ctx.Subps.Element (Ctx.Cur_Subp_Key);
                                                FPR : constant String := Next_Reg (Ctx.E, "afp");
                                             begin
                                                if VI.Kind = VK_Param_Val then
                                                   declare
                                                      Sp : constant String := Next_Reg (Ctx.E, "asp");
                                                   begin
                                                      Emit_Entry_Alloca (Ctx.E, Sp & " = alloca " & To_String (PTI.Pir_Type) & ", align " & Img (Natural'Max (1, PTI.Align)));
                                                      Emit_Inst (Ctx.E, "store." & To_String (PTI.Pir_Type) & " " & To_String (VI.Pir_Name) & ", " & Sp & ", align " & Img (Natural'Max (1, PTI.Align)));
                                                      Emit_Inst (Ctx.E, FPR & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VI.Captured_Slot));
                                                      Emit_Inst (Ctx.E, "store.ptr " & Sp & ", " & FPR & ", align 8");
                                                      VI.Kind     := VK_Local_Alloca;
                                                      VI.Pir_Name := To_Unbounded_String (Sp);
                                                   end;
                                                elsif VI.Kind = VK_Param_Ptr then
                                                   Emit_Inst (Ctx.E, FPR & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VI.Captured_Slot));
                                                   Emit_Inst (Ctx.E, "store.ptr " & To_String (VI.Pir_Name) & ", " & FPR & ", align 8");
                                                elsif VI.Kind = VK_Uncon_Array_Param then
                                                   declare
                                                      DR : constant String := Next_Reg (Ctx.E, "apdr");
                                                      P0 : constant String := Next_Reg (Ctx.E, "apd0");
                                                   begin
                                                      Emit_Entry_Alloca (Ctx.E, DR & " = alloca @__ada_arr_desc, align 8");
                                                      Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & DR & ", 0");
                                                      Emit_Inst (Ctx.E, "store.ptr " & To_String (VI.Pir_Name) & ", " & P0 & ", align 8");
                                                      for D_I in 1 .. Natural'Min (VI.NDims, Max_Array_Dims) loop
                                                         declare
                                                            FF : constant String := Next_Reg (Ctx.E, "apdf");
                                                            LF : constant String := Next_Reg (Ctx.E, "apdl");
                                                         begin
                                                            Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & DR & ", " & Img (2 * D_I - 1));
                                                            Emit_Inst (Ctx.E, "store.i32 " & To_String (VI.First_Regs (D_I)) & ", " & FF & ", align 4");
                                                            Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & DR & ", " & Img (2 * D_I));
                                                            Emit_Inst (Ctx.E, "store.i32 " & To_String (VI.Last_Regs (D_I)) & ", " & LF & ", align 4");
                                                         end;
                                                      end loop;
                                                      Emit_Inst (Ctx.E, FPR & " = field_ptr " & To_String (FSI.Frame_Type) & ", " & To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VI.Captured_Slot));
                                                      Emit_Inst (Ctx.E, "store.ptr " & DR & ", " & FPR & ", align 8");
                                                      VI.Has_Desc := True;
                                                   end;
                                                end if;
                                             end;
                                          end if;
                                          Ctx.Vars.Include (PK, VI);
                                          Ctx.Vars.Include (Tree_Key (PID), VI);
                                          Bound_Ks.Append (PK);
                                       end;
                                    end loop;
                                 end;
                              end loop;
                           end;
                        end if;
                     end;
                  end if;

                  if Has_Body then
                     -- Wrap accept body so unhandled exception propagates to BOTH caller and acceptor (LRM 11.5)
                     declare
                        H_Lbl  : constant String := Next_Label (Ctx.E, "acc_exc");
                        R_Lbl  : constant String := Next_Label (Ctx.E, "acc_ret");
                        E_Lbl  : constant String := Next_Label (Ctx.E, "acc_end");
                        Exn_R  : constant String := Next_Reg (Ctx.E, "aex");
                        H_Mark : Unbounded_String := Null_Unbounded_String;
                     begin
                        if not Ctx.SS_Marks.Is_Empty then H_Mark := Ctx.SS_Marks.Last_Element; end if;
                        Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (H_Lbl),
                                              Current_Exn   => Null_Unbounded_String,
                                              In_Handler    => False,
                                              SS_Mark       => H_Mark,
                                              SP_Mark       => Null_Unbounded_String));
                        Ctx.Accept_End_Lbls.Append (To_Unbounded_String (R_Lbl));
                        Ctx.Accept_Mst_Depths.Append (Unsigned_32 (Ctx.Master_Stack.Length));
                        Emit_Stm_List (Ctx, Acc_SS);
                        Ctx.Accept_Mst_Depths.Delete_Last;
                        Ctx.Accept_End_Lbls.Delete_Last;
                        Ctx.Handlers.Delete_Last;
                        if not Is_Terminated (Ctx.E) then
                           Emit_Term (Ctx.E, "jmp " & R_Lbl);
                        end if;
                        Emit_Label (Ctx.E, R_Lbl);
                        Emit_Call_Or_Invoke (Ctx, "@__ada_accept_end", Self_Reg & ", " & Call_Reg & ", 0", "", Dummy, Nounwind => True);
                        Emit_Term (Ctx.E, "jmp " & E_Lbl);
                        Emit_Label (Ctx.E, H_Lbl, Exn_R & ": ptr");
                        Emit_Call_Or_Invoke (Ctx, "@__ada_accept_end", Self_Reg & ", " & Call_Reg & ", " & Exn_R, "", Dummy, Nounwind => True);
                        Emit_Raise_Reg (Ctx, Exn_R);
                        Emit_Label (Ctx.E, E_Lbl);
                     end;
                  else
                     Emit_Call_Or_Invoke (Ctx, "@__ada_accept_end", Self_Reg & ", " & Call_Reg & ", 0", "", Dummy, Nounwind => True);
                  end if;

                  for BK of Bound_Ks loop
                     if Ctx.Vars.Contains (BK) then
                        declare
                           Old_V : Var_Info := Ctx.Vars.Element (BK);
                        begin
                           if Old_V.Captured_Slot > 0 and then Old_V.Owner_Subp = Ctx.Cur_Subp_Key then
                              Old_V.Pir_Name := Null_Unbounded_String;
                              Ctx.Vars.Replace (BK, Old_V);
                           else
                              Ctx.Vars.Delete (BK);
                           end if;
                        end;
                     end if;
                  end loop;
               end Emit_Accept_Core;

               function Compute_Accept_EIdx (Acc_Node : TREE) return String is
                  E_Name : TREE := D (AS_NAME, Acc_Node);
                  Fam_E  : TREE := CONST_VOID;
                  E_Def  : TREE;
               begin
                  if KIND (E_Name) = DN_INDEXED then
                     declare
                        EL : SEQ_TYPE := LIST (D (AS_EXP_S, E_Name));
                     begin
                        if not IS_EMPTY (EL) then POP (EL, Fam_E); end if;
                     end;
                     E_Name := D (AS_NAME, E_Name);
                  end if;
                  E_Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (E_Name)));
                  if not Is_Void_Tree (E_Def) and then Ctx.Entries.Contains (Tree_Key (E_Def)) then
                     declare
                        EI : constant Entry_Info := Ctx.Entries.Element (Tree_Key (E_Def));
                     begin
                        return Emit_Entry_Index_Reg (Ctx, EI, Fam_E);
                     end;
                  end if;
                  declare
                     C : constant String := Next_Reg (Ctx.E, "aez");
                  begin
                     Emit_Inst (Ctx.E, C & " = const.i32 0");
                     return C;
                  end;
               end Compute_Accept_EIdx;

               Self_R : Unbounded_String := Ctx.Cur_Task_Reg;
            begin
               if Length (Self_R) = 0 then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_task_self", "", "ptr", Self_R, Nounwind => True);
               end if;
               if K = DN_ACCEPT then
                  declare
                     EIdx     : constant String := Compute_Accept_EIdx (Stm);
                     Pkt_Slot : constant String := Next_Reg (Ctx.E, "apks");
                     Call_R   : Unbounded_String;
                     Pkt_R    : constant String := Next_Reg (Ctx.E, "apkr");
                  begin
                     Emit_Entry_Alloca (Ctx.E, Pkt_Slot & " = alloca ptr, align 8");
                     Emit_Call_Or_Invoke (Ctx, "@__ada_accept_begin",
                       To_String (Self_R) & ", " & EIdx & ", " & Pkt_Slot, "ptr", Call_R);
                     Emit_Inst (Ctx.E, Pkt_R & " = load.ptr " & Pkt_Slot & ", align 8");
                     Emit_Accept_Core (Stm, To_String (Self_R), Pkt_R, To_String (Call_R));
                  end;
               else -- DN_SELECTIVE_WAIT
                  declare
                     Clauses : SEQ_TYPE := LIST (D (AS_TEST_CLAUSE_ELEM_S, Stm));
                     C_Node  : TREE;
                     type Sel_Arm is record
                        Alt_Kind : Natural := 0; -- 1=accept, 2=delay, 3=terminate
                        Guard    : TREE := CONST_VOID;
                        First_S  : TREE := CONST_VOID;
                        Stm_S    : TREE := CONST_VOID;
                        Arm_Lbl  : Unbounded_String;
                     end record;
                     Arms : array (0 .. 63) of Sel_Arm;
                     NArm : Natural := 0;
                     Else_SS  : constant TREE := D (AS_STM_S, Stm);
                     Has_Else : constant Boolean := not Is_Void_Tree (Else_SS) and then not IS_EMPTY (LIST (Else_SS));
                  begin
                     while not IS_EMPTY (Clauses) and then NArm < 64 loop
                        POP (Clauses, C_Node);
                        if KIND (C_Node) = DN_SELECT_ALTERNATIVE then
                           declare
                              SS : constant TREE := D (AS_STM_S, C_Node);
                              SL : SEQ_TYPE := (if Is_Void_Tree (SS) then MAKE else LIST (SS));
                              FS : TREE := CONST_VOID;
                           begin
                              while not IS_EMPTY (SL) loop
                                 POP (SL, FS);
                                 exit when KIND (FS) /= DN_STM_PRAGMA;
                              end loop;
                              Arms (NArm).Guard   := D (AS_EXP, C_Node);
                              Arms (NArm).First_S := FS;
                              Arms (NArm).Stm_S   := SS;
                              Arms (NArm).Arm_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "sw_a"));
                              if not Is_Void_Tree (FS) then
                                 if KIND (FS) = DN_ACCEPT then Arms (NArm).Alt_Kind := 1;
                                 elsif KIND (FS) = DN_DELAY then Arms (NArm).Alt_Kind := 2;
                                 elsif KIND (FS) = DN_TERMINATE then Arms (NArm).Alt_Kind := 3;
                                 end if;
                              end if;
                              NArm := NArm + 1;
                           end;
                        end if;
                     end loop;

                     declare
                        AK_Arr   : constant String := Next_Reg (Ctx.E, "swk");
                        EI_Arr   : constant String := Next_Reg (Ctx.E, "swe");
                        DL_Arr   : constant String := Next_Reg (Ctx.E, "swd");
                        Pkt_Slot : constant String := Next_Reg (Ctx.E, "swps");
                        Cal_Slot : constant String := Next_Reg (Ctx.E, "swcs");
                        Chosen   : Unbounded_String;
                        L_End    : constant String := Next_Label (Ctx.E, "sw_end");
                        L_Else   : constant String := Next_Label (Ctx.E, "sw_els");
                        L_Term   : constant String := Next_Label (Ctx.E, "sw_trm");
                        Sw_Str   : Unbounded_String;
                     begin
                        Add_Type_Decl (Ctx.E, "type @__ada_sw_i32 = [64 x i32]");
                        Add_Type_Decl (Ctx.E, "type @__ada_sw_i64 = [64 x i64]");
                        Emit_Entry_Alloca (Ctx.E, AK_Arr & " = alloca @__ada_sw_i32, align 4");
                        Emit_Entry_Alloca (Ctx.E, EI_Arr & " = alloca @__ada_sw_i32, align 4");
                        Emit_Entry_Alloca (Ctx.E, DL_Arr & " = alloca @__ada_sw_i64, align 8");
                        Emit_Entry_Alloca (Ctx.E, Pkt_Slot & " = alloca ptr, align 8");
                        Emit_Entry_Alloca (Ctx.E, Cal_Slot & " = alloca ptr, align 8");

                        -- P3-4.15: LRM 9.7.1(5) Pass 1: evaluate ALL guards before any delay/entry-family index expressions
                        for I in 0 .. NArm - 1 loop
                           declare
                              KP : constant String := Next_Reg (Ctx.E, "swkp");
                              EP : constant String := Next_Reg (Ctx.E, "swep");
                              DP : constant String := Next_Reg (Ctx.E, "swdp");
                           begin
                              Emit_Inst (Ctx.E, KP & " = elem_ptr i32, " & AK_Arr & ", " & Img (I));
                              Emit_Inst (Ctx.E, EP & " = elem_ptr i32, " & EI_Arr & ", " & Img (I));
                              Emit_Inst (Ctx.E, DP & " = elem_ptr i64, " & DL_Arr & ", " & Img (I));
                              Emit_Inst (Ctx.E, "store.i32 0, " & EP & ", align 4");
                              Emit_Inst (Ctx.E, "store.i64 0, " & DP & ", align 8");
                              if not Is_Void_Tree (Arms (I).Guard) then
                                 declare
                                    GV  : constant Expr_Val := Emit_Expr (Ctx, Arms (I).Guard);
                                    Sel : constant String := Next_Reg (Ctx.E, "swks");
                                 begin
                                    Emit_Inst (Ctx.E, Sel & " = select.i32 " & To_String (GV.Reg) & ", " & Img (Arms (I).Alt_Kind) & ", 0");
                                    Emit_Inst (Ctx.E, "store.i32 " & Sel & ", " & KP & ", align 4");
                                 end;
                              else
                                 Emit_Inst (Ctx.E, "store.i32 " & Img (Arms (I).Alt_Kind) & ", " & KP & ", align 4");
                              end if;
                           end;
                        end loop;

                        -- Pass 2: evaluate entry-family indices and delay durations ONLY for open alternatives
                        for I in 0 .. NArm - 1 loop
                           if Arms (I).Alt_Kind = 1 or else Arms (I).Alt_Kind = 2 then
                              declare
                                 KP : constant String := Next_Reg (Ctx.E, "swk2");
                                 EP : constant String := Next_Reg (Ctx.E, "swe2");
                                 DP : constant String := Next_Reg (Ctx.E, "swd2");
                              begin
                                 Emit_Inst (Ctx.E, KP & " = elem_ptr i32, " & AK_Arr & ", " & Img (I));
                                 Emit_Inst (Ctx.E, EP & " = elem_ptr i32, " & EI_Arr & ", " & Img (I));
                                 Emit_Inst (Ctx.E, DP & " = elem_ptr i64, " & DL_Arr & ", " & Img (I));
                                 if not Is_Void_Tree (Arms (I).Guard) then
                                    declare
                                       KV    : constant String := Next_Reg (Ctx.E, "swkv");
                                       Is_Op : constant String := Next_Reg (Ctx.E, "swop");
                                       L_Opn : constant String := Next_Label (Ctx.E, "swg_o");
                                       L_Cls : constant String := Next_Label (Ctx.E, "swg_c");
                                    begin
                                       Emit_Inst (Ctx.E, KV & " = load.i32 " & KP & ", align 4");
                                       Emit_Inst (Ctx.E, Is_Op & " = cmp_ne.i32 " & KV & ", 0");
                                       Emit_Term (Ctx.E, "br " & Is_Op & ", " & L_Opn & ", " & L_Cls);
                                       Emit_Label (Ctx.E, L_Opn);
                                       if Arms (I).Alt_Kind = 1 then
                                          Emit_Inst (Ctx.E, "store.i32 " & Compute_Accept_EIdx (Arms (I).First_S) & ", " & EP & ", align 4");
                                       else
                                          Emit_Inst (Ctx.E, "store.i64 " & Emit_Duration_Ns (Ctx, D (AS_EXP, Arms (I).First_S)) & ", " & DP & ", align 8");
                                       end if;
                                       Emit_Term (Ctx.E, "jmp " & L_Cls);
                                       Emit_Label (Ctx.E, L_Cls);
                                    end;
                                 else
                                    if Arms (I).Alt_Kind = 1 then
                                       Emit_Inst (Ctx.E, "store.i32 " & Compute_Accept_EIdx (Arms (I).First_S) & ", " & EP & ", align 4");
                                    else
                                       Emit_Inst (Ctx.E, "store.i64 " & Emit_Duration_Ns (Ctx, D (AS_EXP, Arms (I).First_S)) & ", " & DP & ", align 8");
                                    end if;
                                 end if;
                              end;
                           end if;
                        end loop;

                        Emit_Call_Or_Invoke (Ctx, "@__ada_select_wait",
                          To_String (Self_R) & ", " & Img (NArm) & ", " &
                          AK_Arr & ", " & EI_Arr & ", " & DL_Arr & ", " &
                          (if Has_Else then "1" else "0") & ", " & Pkt_Slot & ", " & Cal_Slot,
                          "i32", Chosen);

                        Append (Sw_Str, "-2 -> " & L_Term);
                        if Has_Else then
                           Append (Sw_Str, ", -1 -> " & L_Else);
                        end if;
                        for I in 0 .. NArm - 1 loop
                           Append (Sw_Str, ", " & Img (I) & " -> " & To_String (Arms (I).Arm_Lbl));
                        end loop;
                        Emit_Term (Ctx.E, "switch.i32 " & To_String (Chosen) & ", default " & L_End & " [ " & To_String (Sw_Str) & " ]");

                        Emit_Label (Ctx.E, L_Term);
                        if Length (Ctx.Cur_Task_Exit_Lbl) > 0 then
                           Emit_Master_Leave_To (Ctx, Ctx.Cur_Task_Base_Mst_Depth);
                           Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));
                        else
                           Emit_Term (Ctx.E, "ret");
                        end if;

                        if Has_Else then
                           Emit_Label (Ctx.E, L_Else);
                           Emit_Stm_List (Ctx, Else_SS);
                           if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        end if;

                        for I in 0 .. NArm - 1 loop
                           Emit_Label (Ctx.E, To_String (Arms (I).Arm_Lbl));
                           declare
                              SL : SEQ_TYPE := (if Is_Void_Tree (Arms (I).Stm_S) then MAKE else LIST (Arms (I).Stm_S));
                              S_It : TREE;
                              Past_First : Boolean := False;
                           begin
                              while not IS_EMPTY (SL) loop
                                 POP (SL, S_It);
                                 if not Past_First then
                                    if S_It = Arms (I).First_S then
                                       Past_First := True;
                                       if Arms (I).Alt_Kind = 1 then
                                          declare
                                             PR : constant String := Next_Reg (Ctx.E, "swpr");
                                             CR : constant String := Next_Reg (Ctx.E, "swcr");
                                          begin
                                             Emit_Inst (Ctx.E, PR & " = load.ptr " & Pkt_Slot & ", align 8");
                                             Emit_Inst (Ctx.E, CR & " = load.ptr " & Cal_Slot & ", align 8");
                                             Emit_Accept_Core (Arms (I).First_S, To_String (Self_R), PR, CR);
                                          end;
                                       end if;
                                    end if;
                                 else
                                    if not Is_Terminated (Ctx.E) or else KIND (S_It) = DN_LABELED then
                                       Emit_Stm (Ctx, S_It);
                                    end if;
                                 end if;
                              end loop;
                           end;
                           if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & L_End); end if;
                        end loop;
                        Emit_Label (Ctx.E, L_End);
                     end;
                  end;
               end if;
            end;

         when others =>
            Emit_Comment (Ctx.E, "Unhandled statement: " & NODE_IMAGE (K));
      end case;
   end Emit_Stm;

   -- =========================================================================
   -- Subprogram Body Emission (with Static-Link Frame Setup)
   -- =========================================================================

   procedure Emit_Subprogram_Body
     (Ctx           : in out Context;
      Body_Node     : TREE;
      Override_Id   : TREE := CONST_VOID;
      Override_Spec : TREE := CONST_VOID;
      Is_Exported   : Boolean := False;
      Is_Subunit    : Boolean := False)
   is
      Subp_Id   : constant TREE :=
        (if not Is_Void_Tree (Override_Id) then Canonical_Def (Override_Id)
         else Canonical_Def (Subst_Tree (Ctx, Canonical_Def (D (AS_SOURCE_NAME, Body_Node)))));
      Spec_Node : constant TREE :=
        (if not Is_Void_Tree (Override_Spec) then Override_Spec
         elsif Subp_Id /= Canonical_Def (D (AS_SOURCE_NAME, Body_Node))
           and then not Is_Void_Tree (D (SM_SPEC, Subp_Id))
         then D (SM_SPEC, Subp_Id)
         else D (AS_HEADER, Body_Node));
      Subp_Key  : constant Unsigned_32 := Tree_Key (Subp_Id);
      Block_B   : constant TREE := D (AS_BODY, Body_Node);
      SI        : Subp_Info;

      -- Save outer subprogram context (to support emitting nested subprograms cleanly)
      Saved_Buf      : Pir_Emit.Func_Buffer;
      Saved_Key      : constant Unsigned_32 := Ctx.Cur_Subp_Key;
      Saved_Node     : constant TREE := Ctx.Cur_Subp_Node;
      Saved_Ret_T     : constant TREE := Ctx.Cur_Ret_Type;
      Saved_Ret_Comp  : constant Boolean := Ctx.Cur_Ret_Comp;
      Saved_Ret_Uncon : constant Boolean := Ctx.Cur_Ret_Uncon;
      Saved_Ret_Ptr   : constant Unbounded_String := Ctx.Cur_Ret_Ptr;
      Saved_Frame     : constant Unbounded_String := Ctx.Cur_Frame_Reg;
      Saved_SL        : constant Unbounded_String := Ctx.Cur_SL_Reg;
      Saved_Mst       : constant Unbounded_String := Ctx.Cur_Master_Reg;
      Saved_Tsk       : constant Unbounded_String := Ctx.Cur_Task_Reg;
      Saved_TExit     : constant Unbounded_String := Ctx.Cur_Task_Exit_Lbl;
      Saved_Is_Func   : constant Boolean := Ctx.Cur_Is_Func;
      Saved_Loops       : constant Loop_Vecs.Vector := Ctx.Loops;
      Saved_Handlers    : constant Handler_Vecs.Vector := Ctx.Handlers;
      Saved_Copy_Outs   : constant Copy_Out_Vecs.Vector := Ctx.Copy_Outs;
      Saved_SS_Marks    : constant Str_Vecs.Vector := Ctx.SS_Marks;
      Saved_SP_Marks    : constant Str_Vecs.Vector := Ctx.SP_Marks;
      Saved_Mst_Stack   : constant Str_Vecs.Vector := Ctx.Master_Stack;
      Saved_Acc_Lbls    : constant Str_Vecs.Vector := Ctx.Accept_End_Lbls;
      Saved_Acc_Mst_Dep : constant U32_Vecs.Vector := Ctx.Accept_Mst_Depths;
      Saved_TBase_Mst   : constant Natural := Ctx.Cur_Task_Base_Mst_Depth;
      Saved_Lbl_Mst_Dep : constant I64_Maps.Map := Ctx.Label_Mst_Depths;
      Saved_Lbl_SS_Dep  : constant I64_Maps.Map := Ctx.Label_SS_Depths;
      Saved_Lbl_SP_Dep  : constant I64_Maps.Map := Ctx.Label_SP_Depths;
      Saved_Uplevel     : constant Type_Maps.Map := Ctx.Uplevel_Cache;

      Captured_Vars  : Cap_Vecs.Vector;
      Nested_Subps   : U32_Vecs.Vector;
   begin
      declare
         Bdy_Hdr : constant TREE := D (AS_HEADER, Body_Node);
      begin
         if not Is_Void_Tree (Bdy_Hdr) and then not Is_Void_Tree (Spec_Node) and then Bdy_Hdr /= Spec_Node then
            declare
               BPS : constant TREE := D (AS_PARAM_S, Bdy_Hdr);
               SPS : constant TREE := D (AS_PARAM_S, Spec_Node);
            begin
               if not Is_Void_Tree (BPS) and then not Is_Void_Tree (SPS) then
                  declare
                     BL : SEQ_TYPE := LIST (BPS);
                     SL : SEQ_TYPE := LIST (SPS);
                     BP, SP : TREE;
                  begin
                     while not IS_EMPTY (BL) and then not IS_EMPTY (SL) loop
                        POP (BL, BP); POP (SL, SP);
                        declare
                           BNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, BP));
                           SNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, SP));
                           BID, SID_N : TREE;
                        begin
                           while not IS_EMPTY (BNS) and then not IS_EMPTY (SNS) loop
                              POP (BNS, BID); POP (SNS, SID_N);
                              Ctx.Generic_Subst.Include (Tree_Key (Canonical_Def (BID)), Canonical_Def (SID_N));
                              Ctx.Generic_Subst.Include (Tree_Key (BID), Canonical_Def (SID_N));
                           end loop;
                        end;
                     end loop;
                  end;
               end if;
            end;
         end if;
      end;

      -- P3-5.3: Inline separate subunit belonging to a generic template BEFORE Eff_Subunit / Needs_SL:=False!
      if not Is_Void_Tree (Block_B) and then KIND (Block_B) = DN_STUB then
         declare
            Sub_Body : constant TREE := Find_Gen_Subunit_Body (Ctx, Body_Node);
         begin
            if not Is_Void_Tree (Sub_Body) and then KIND (Sub_Body) = DN_SUBPROGRAM_BODY
              and then not Is_Void_Tree (D (AS_BODY, Sub_Body))
              and then KIND (D (AS_BODY, Sub_Body)) /= DN_STUB
            then
               Emit_Subprogram_Body (Ctx, Sub_Body,
                 Override_Id   => Subp_Id,
                 Override_Spec => Spec_Node,
                 Is_Exported   => Is_Exported,
                 Is_Subunit    => Is_Subunit);
               return;
            end if;
         end;
      end if;

      declare
         Was_Pre_Reg : constant Boolean := Ctx.Subps.Contains (Subp_Key);
         Eff_Subunit : constant Boolean := Is_Subunit or else Ctx.Has_Stubs or else (not Is_Void_Tree (Block_B) and then KIND (Block_B) = DN_STUB);
      begin
         Pre_Register_Subp (Ctx, Subp_Id, Spec_Node, Saved_Key, Is_Subunit => Eff_Subunit);
         SI := Ctx.Subps.Element (Subp_Key);
         if not Is_Void_Tree (Block_B) and then KIND (Block_B) = DN_STUB then
            SI.Needs_SL := False;
            Ctx.Subps.Replace (Subp_Key, SI);
         end if;
         if not Was_Pre_Reg and then not Eff_Subunit and then Length (Ctx.Gen_Inst_Tag) > 0 and then not Is_Library_Level (Subp_Id) then
            SI.Pir_Name := To_Unbounded_String (Mangle_Subp_Name (Ctx, Subp_Id, Spec_Node));
            Ctx.Subps.Replace (Subp_Key, SI);
         end if;
      end;

      -- Handle external separate subunit stub: declare extern and skip
      if Is_Void_Tree (Block_B) or else KIND (Block_B) = DN_STUB then
         Ensure_Extern_Subp (Ctx, Subp_Id, SI);
         return;
      end if;

      -- Pre-register local variables, block variables, loop variables, & parameters for static-link capture scan
      declare
         Temp_Owners : Var_Maps.Map;
         Local_Meta  : U32_Vecs.Vector;
         PS : constant TREE := D (AS_PARAM_S, Spec_Node);

         procedure Collect_Local_Owners (Node : TREE; In_Gen_Bdy : Boolean := False) is
            K : NODE_NAME;
         begin
            if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then
               return;
            end if;
            K := KIND (Node);
            if K = DN_SUBPROGRAM_BODY or else K = DN_PACKAGE_BODY then
               declare
                  SN : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
               begin
                  if not Is_Void_Tree (SN) and then KIND (SN) = DN_GENERIC_ID then
                     if not Ctx.Generic_Body.Contains (Tree_Key (SN))
                       or else Is_Void_Tree (D (AS_BODY, Node))
                       or else KIND (D (AS_BODY, Node)) /= DN_STUB
                     then
                        Ctx.Generic_Body.Include (Tree_Key (SN), Node);
                     end if;
                  end if;
               end;
                  if K = DN_SUBPROGRAM_BODY then
                     return;
                  end if;
                  if K = DN_PACKAGE_BODY then
                     declare
                        Sub_P : constant TREE := Find_Gen_Subunit_Body (Ctx, Node);
                     begin
                        if not Is_Void_Tree (Sub_P) then
                           Collect_Local_Owners (D (AS_BODY, Sub_P), In_Gen_Bdy);
                           return;
                        end if;
                     end;
                  end if;
            end if;
            if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then
               return;
            end if;
            if K = DN_TYPE_DECL then
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS  : constant TREE :=
                    (if not Is_Void_Tree (TID) and then KIND (TID) in CLASS_TYPE_NAME
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TID))) else CONST_VOID);
               begin
                  if not Is_Void_Tree (TS) and then KIND (TS) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                    and then Type_Contains_Task (Ctx, D (SM_DESIG_TYPE, TS))
                  then
                     declare
                        BS : constant TREE := Base_Type_Spec (TS);
                     begin
                        if not Is_Void_Tree (BS) and then KIND (BS) = DN_ACCESS and then Is_Void_Tree (D (SM_DERIVED, BS)) then
                           Local_Meta.Append (Tree_Key (BS));
                        end if;
                     end;
                  end if;
               end;
               return;
            elsif K = DN_VARIABLE_DECL or else K = DN_CONSTANT_DECL then
               declare
                  NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Node));
                  ID : TREE;
               begin
                  while not IS_EMPTY (NS) loop
                     POP (NS, ID);
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := Subp_Key;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                     end;
                  end loop;
               end;
               return;
            elsif K = DN_TASK_DECL then
               declare
                  Src : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS_M : TREE := CONST_VOID;
               begin
                  if not Is_Void_Tree (Src) and then KIND (Src) = DN_VARIABLE_ID then
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := Subp_Key;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (Src)), V);
                     end;
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src)));
                  elsif not Is_Void_Tree (Src) and then KIND (Src) in CLASS_TYPE_NAME then
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Src)));
                  end if;
                  if not Is_Void_Tree (TS_M) and then KIND (TS_M) = DN_TASK_SPEC then
                     Local_Meta.Append (Tree_Key (TS_M));
                  end if;
               end;
               return;
            elsif K = DN_LENGTH_ENUM_REP then
               -- P3-6.6: Capture local dynamic 'STORAGE_SIZE slots in parent static-link frame
               declare
                  AN : constant TREE := D (AS_NAME, Node);
                  EN : constant TREE := D (AS_EXP, Node);
               begin
                  if not Is_Void_Tree (AN) and then KIND (AN) = DN_ATTRIBUTE and then not Is_Void_Tree (EN)
                    and then not Is_Void_Tree (D (AS_USED_NAME_ID, AN))
                    and then Sym_Name (D (AS_USED_NAME_ID, AN)) = "STORAGE_SIZE"
                  then
                     Local_Meta.Append (Tree_Key (EN));
                  end if;
               end;
               return;
            elsif K = DN_RENAMES_OBJ_DECL then
               declare
                  ID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := Subp_Key;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
               return;
            elsif K = DN_FOR or else K = DN_REVERSE then
               declare
                  ID : constant TREE := D (AS_SOURCE_NAME, Node);
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := Subp_Key;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
               return;
            elsif K = DN_SUBPROG_ENTRY_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     Collect_Local_Owners (D (SM_DECL_S, UK), In_Gen_Bdy);
                     return;
                  elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                     declare
                        SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        V   : Var_Info;
                     begin
                        V.Owner_Subp    := Subp_Key;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (SID), V);
                     end;
                     return;
                  end if;
               end;
            elsif K = DN_PACKAGE_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     declare
                        PID    : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                        GK     : constant Unsigned_32 := Tree_Key (Gen_Id);
                        GB     : TREE := CONST_VOID;
                     begin
                        Collect_Local_Owners (D (SM_DECL_S, UK), False);
                        Collect_Local_Owners (D (SM_SPEC, PID), False);
                        -- P3-5.1: Propagate nested generic bodies in instantiated package spec
                        declare
                           GSp : constant TREE := (if not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_SPEC, Gen_Id) else CONST_VOID);
                           ISp : constant TREE := D (SM_SPEC, PID);
                           Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
                        begin
                           if not Is_Void_Tree (GSp) and then KIND (GSp) = DN_PACKAGE_SPEC
                             and then not Is_Void_Tree (ISp) and then KIND (ISp) = DN_PACKAGE_SPEC
                           then
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, GSp), D (AS_DECL_S1, ISp));
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, GSp), D (AS_DECL_S2, ISp));
                           end if;
                           Ctx.Generic_Subst := Saved_Sub;
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
                              Collect_Local_Owners (D (AS_BODY, GB), True);
                           else
                              Collect_Local_Owners (GB, True);
                           end if;
                        end if;
                        return;
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
                     Collect_Local_Owners (Item, In_Gen_Bdy);
                  end loop;
               end;
            else
               for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
                  Collect_Local_Owners (DABS (I, Node), In_Gen_Bdy);
               end loop;
            end if;
         end Collect_Local_Owners;
      begin
         if not Is_Void_Tree (PS) then
            declare
               PL : SEQ_TYPE := LIST (PS);
               P  : TREE;
            begin
               while not IS_EMPTY (PL) loop
                  POP (PL, P);
                  declare
                     NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                     ID : TREE;
                  begin
                     while not IS_EMPTY (NS) loop
                        POP (NS, ID);
                        declare
                           V : Var_Info;
                        begin
                           V.Owner_Subp := Subp_Key;
                           Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                        end;
                     end loop;
                  end;
               end loop;
            end;
         end if;
         Collect_Local_Owners (Block_B);
         Scan_Tree_For_Captures (Ctx, Block_B, Subp_Key, Temp_Owners, Ctx.Subps, Captured_Vars, Nested_Subps, Subp_Key);
         -- P3-4.3: Capture local access-master & task-elab slots in frame whenever nested subprograms/tasks exist
         if not Local_Meta.Is_Empty and then (not Nested_Subps.Is_Empty or else Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Block_B))) then
            for MK of Local_Meta loop
               declare
                  Found_M : Boolean := False;
               begin
                  for CP of Captured_Vars loop
                     if CP.Inst_Key = 0 and then CP.Var_Key = MK then
                        Found_M := True;
                        exit;
                     end if;
                  end loop;
                  if not Found_M then
                     Captured_Vars.Append ((Inst_Key => 0, Var_Key => MK));
                  end if;
               end;
            end loop;
         end if;
      end;

      -- Reload SI (Needs_SL might have been set by outer scan!)
      SI := Ctx.Subps.Element (Subp_Key);
      if not Captured_Vars.Is_Empty or else SI.Needs_SL then
         -- Propagate Needs_SL to all nested subprograms inside this subprogram so sibling/grandchild calls always have a valid SL!
         for NK of Nested_Subps loop
            if Ctx.Subps.Contains (NK) then
               declare
                  NSI : Subp_Info := Ctx.Subps.Element (NK);
               begin
                  NSI.Needs_SL := True;
                  Ctx.Subps.Replace (NK, NSI);
               end;
            end if;
         end loop;
      end if;
      if not Captured_Vars.Is_Empty or else (SI.Needs_SL and then not Nested_Subps.Is_Empty) then
         SI.Has_Frame := True;
         SI.Frame_Type := To_Unbounded_String ("@__frame_" & Tree_Key_Str (Subp_Id) & To_String (Ctx.Gen_Inst_Tag));
         -- Emit frame struct: { ptr parent_sl, ptr cap1..capN, ptr con1..conN }
         declare
            N_Cap     : constant Natural := Natural (Captured_Vars.Length);
            FT_Decl   : Unbounded_String := To_Unbounded_String ("type " & To_String (SI.Frame_Type) & " = { ptr");
         begin
            for Slot in 1 .. N_Cap loop
               Append (FT_Decl, ", ptr");
            end loop;
            for Slot in 1 .. N_Cap loop
               Append (FT_Decl, ", ptr");
               declare
                  CP : constant Cap_Pair := Captured_Vars.Element (Slot - 1);
                  CK : constant Unsigned_32 := CP.Var_Key;
               begin
                  if CP.Inst_Key /= 0 then
                     Ctx.Gen_Cap_Slots.Include
                       (Shift_Left (Unsigned_64 (CP.Inst_Key), 32) or Unsigned_64 (CK), Slot);
                  else
                     declare
                        V : Var_Info;
                     begin
                        if Ctx.Vars.Contains (CK) then
                           V := Ctx.Vars.Element (CK);
                        end if;
                        V.Captured_Slot     := Slot;
                        V.Con_Captured_Slot := N_Cap + Slot;
                        V.Owner_Subp        := Subp_Key;
                        Ctx.Vars.Include (CK, V);
                     end;
                  end if;
               end;
            end loop;
            Append (FT_Decl, " }");
            Add_Type_Decl (Ctx.E, To_String (FT_Decl));
         end;
         Ctx.Subps.Replace (Subp_Key, SI);
      end if;

      -- Build function header
      declare
         Link   : constant String := (if Is_Exported then "export " else "");
         Params : constant String :=
           Build_Param_Sig (Ctx, Spec_Node, SI.Needs_SL, SI.Ret_Composite, SI.Ret_Type, True);
         Ret_S  : constant String :=
           (if not SI.Is_Function or else SI.Ret_Composite then "void"
            else To_String (Get_Type_Info (Ctx, SI.Ret_Type).Pir_Type));
         Hdr    : constant String :=
           Link & "func " & To_String (SI.Pir_Name) & "(" & Params & ") -> " & Ret_S & " cc ""internal""";
      begin
         Begin_Func (Ctx.E, Saved_Buf, Hdr);
      end;

      Ctx.Cur_Subp_Key  := Subp_Key;
      Ctx.Cur_Subp_Node := Body_Node;
      Ctx.Cur_Is_Func   := SI.Is_Function;
      Ctx.Cur_Ret_Type  := SI.Ret_Type;
      Ctx.Cur_Ret_Comp  := SI.Ret_Composite;
      Ctx.Cur_Ret_Uncon := SI.Ret_Uncon_Arr;
      Ctx.Cur_Ret_Ptr   := To_Unbounded_String (if SI.Ret_Composite then "%__ret" else "");
      Ctx.Cur_SL_Reg        := To_Unbounded_String (if SI.Needs_SL then "%__sl" else "");
      Ctx.Cur_Frame_Reg     := Null_Unbounded_String;
      Ctx.Cur_Master_Reg    := Null_Unbounded_String;
      Ctx.Cur_Task_Reg            := Null_Unbounded_String;
      Ctx.Cur_Task_Exit_Lbl       := Null_Unbounded_String;
      Ctx.Cur_Task_Base_Mst_Depth := 0;
      Ctx.Loops.Clear;
      Ctx.Handlers.Clear;
      Ctx.Copy_Outs.Clear;
      Ctx.SS_Marks.Clear;
      Ctx.SP_Marks.Clear;
      Ctx.Master_Stack.Clear;
      Ctx.Accept_End_Lbls.Clear;
      Ctx.Accept_Mst_Depths.Clear;
      Ctx.Label_Mst_Depths.Clear;
      Ctx.Label_SS_Depths.Clear;
      Ctx.Label_SP_Depths.Clear;
      Ctx.Uplevel_Cache.Clear;
      if not Is_Void_Tree (D (AS_BODY, Body_Node)) and then KIND (D (AS_BODY, Body_Node)) = DN_BLOCK_BODY then
         declare
            BB     : constant TREE := D (AS_BODY, Body_Node);
            InitD  : constant Natural := (if Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, BB)) then 1 else 0);
            InitSS : constant Natural := (if not SI.Ret_Uncon_Arr and then Tree_Uses_SS (Ctx, BB) then 1 else 0);
         begin
            Scan_Label_Master_Depths (Ctx, D (AS_STM_S, BB), InitD, InitSS, 0);
            Scan_Label_Master_Depths (Ctx, D (AS_ALTERNATIVE_S, BB), InitD, InitSS, 0);
            -- P3-4.14: Apply pragma PRIORITY for a main subprogram (LRM 9.8(2))
            if Is_Exported and then not Is_Subunit and then Is_Library_Level (Subp_Id)
              and then not Is_Void_Tree (D (AS_ITEM_S, BB))
            then
               declare
                  IL : SEQ_TYPE := LIST (D (AS_ITEM_S, BB));
                  It : TREE;
               begin
                  while not IS_EMPTY (IL) loop
                     POP (IL, It);
                     if KIND (It) = DN_PRAGMA then
                        declare
                           PID : constant TREE := D (AS_USED_NAME_ID, It);
                           PAS : constant TREE := D (AS_GENERAL_ASSOC_S, It);
                        begin
                           if not Is_Void_Tree (PID) and then Sym_Name (PID) = "PRIORITY"
                             and then not Is_Void_Tree (PAS) and then not IS_EMPTY (LIST (PAS))
                           then
                              declare
                                 A1 : TREE := HEAD (LIST (PAS));
                                 SV : TREE;
                                 Dummy_P : Unbounded_String;
                              begin
                                 if KIND (A1) = DN_ASSOC then A1 := D (AS_EXP, A1); end if;
                                 SV := Safe_SM_Value (A1);
                                 if Has_Static_Val (SV) and then KIND (SV) = DN_NUM_VAL then
                                    declare
                                       PV : constant Integer_64 := Eval_Int64 (SV);
                                    begin
                                       if PV >= 0 and then PV <= 10 then
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_set_main_priority", Img (Long_Long_Integer (PV)), "", Dummy_P, Nounwind => True);
                                       end if;
                                    end;
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                  end loop;
               end;
            end if;
         end;
      end if;

      -- LRM 3.9: check subprogram body has been elaborated before call (c39006*, ca5006a)
      if Ctx.Enable_Checks and then Length (SI.Elab_Flag) > 0 then
         declare
            FA    : constant String := Next_Reg (Ctx.E, "sfa");
            FV    : constant String := Next_Reg (Ctx.E, "sfv");
            NE    : constant String := Next_Reg (Ctx.E, "sne");
            L_Bad : constant String := Next_Label (Ctx.E, "spe_b");
            L_Ok  : constant String := Next_Label (Ctx.E, "spe_o");
         begin
            Emit_Inst (Ctx.E, FA & " = addr " & To_String (SI.Elab_Flag));
            Emit_Inst (Ctx.E, FV & " = load.i8 " & FA & ", align 1");
            Emit_Inst (Ctx.E, NE & " = cmp_eq.i8 " & FV & ", 0");
            Emit_Term (Ctx.E, "br " & NE & ", " & L_Bad & ", " & L_Ok);
            Emit_Label (Ctx.E, L_Bad);
            Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
            Emit_Label (Ctx.E, L_Ok);
         end;
      end if;

      if SI.Has_Frame then
         declare
            FR : constant String := Next_Reg (Ctx.E, "fr");
            FP : constant String := Next_Reg (Ctx.E, "frp");
         begin
            Emit_Entry_Alloca (Ctx.E, FR & " = alloca " & To_String (SI.Frame_Type) & ", align 8");
            Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " & FR & ", 0");
            if SI.Needs_SL then
               Emit_Inst (Ctx.E, "store.ptr %__sl, " & FP & ", align 8");
            else
               Emit_Inst (Ctx.E, "store.ptr 0, " & FP & ", align 8");
            end if;
            Ctx.Cur_Frame_Reg := To_Unbounded_String (FR);
         end;
      end if;

      -- Bind formal parameters into Ctx.Vars
      -- If this subprogram declares direct separate subunits (DN_STUB), export scalar
      -- formal parameters via Dijkstra-display globals (saved on entry, restored on exit)
      -- so child subunits can access both non-recursive and recursive frames!
      declare
         Subp_Has_Stubs : Boolean := False;
      begin
         if not Is_Void_Tree (Block_B) and then KIND (Block_B) /= DN_STUB then
            declare
               IS_N : constant TREE := D (AS_ITEM_S, Block_B);
            begin
               if not Is_Void_Tree (IS_N) then
                  declare
                     SL : SEQ_TYPE := LIST (IS_N);
                     IT : TREE;
                  begin
                     while not IS_EMPTY (SL) loop
                        POP (SL, IT);
                        if (KIND (IT) = DN_SUBPROGRAM_BODY or else KIND (IT) = DN_PACKAGE_BODY or else KIND (IT) = DN_TASK_BODY)
                          and then not Is_Void_Tree (D (AS_BODY, IT))
                          and then KIND (D (AS_BODY, IT)) = DN_STUB
                          and then not Is_Gen_Subunit_Stub (Ctx, IT)
                        then
                           Subp_Has_Stubs := True;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;
            end;
         end if;

      if not Is_Void_Tree (Spec_Node) then
         declare
            PS  : constant TREE := D (AS_PARAM_S, Spec_Node);
            PL  : SEQ_TYPE := (if Is_Void_Tree (PS) then MAKE else LIST (PS));
            P   : TREE;
            Idx : Natural := 0;
         begin
            while not IS_EMPTY (PL) loop
               POP (PL, P);
               declare
                  Mode : constant NODE_NAME := KIND (P);
                  NS   : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                  PID  : TREE;
               begin
                  while not IS_EMPTY (NS) loop
                     POP (NS, PID);
                     declare
                        Can_P : constant TREE := Canonical_Def (PID);
                        PK    : constant Unsigned_32 := Tree_Key (Can_P);
                        PT    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, PID));
                        TI    : constant Type_Info := Get_Type_Info (Ctx, PT);
                        PN    : constant String := "%p_" & Sanitize_Ident (Sym_Name (PID)) & "_" & Img (Idx);
                        VInfo : Var_Info;
                     begin
                        if Ctx.Vars.Contains (PK) and then Ctx.Vars.Element (PK).Owner_Subp = Subp_Key then
                           VInfo.Captured_Slot     := Ctx.Vars.Element (PK).Captured_Slot;
                           VInfo.Con_Captured_Slot := Ctx.Vars.Element (PK).Con_Captured_Slot;
                        end if;
                        VInfo.Ada_Type   := PT;
                        VInfo.Pir_Type   := TI.Pir_Type;
                        VInfo.Align      := TI.Align;
                        VInfo.Owner_Subp := Subp_Key;

                        if TI.Kind = TK_Unconstrained_Array
                          or else (TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic)
                        then
                           VInfo.Kind     := VK_Uncon_Array_Param;
                           VInfo.Pir_Name := To_Unbounded_String (PN & "_ptr");
                           VInfo.NDims    := TI.NDims;
                           for D_Idx in 1 .. TI.NDims loop
                              VInfo.First_Regs (D_Idx) := To_Unbounded_String (PN & "_f" & Img (D_Idx));
                              VInfo.Last_Regs (D_Idx)  := To_Unbounded_String (PN & "_l" & Img (D_Idx));
                           end loop;
                           if Subp_Has_Stubs then
                              -- Export array descriptor display so separate subunits can access parent unconstrained array params
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "spda");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spds");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global @__ada_arr_desc = zeroinit, align 8");
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca @__ada_arr_desc, align 8");
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, "memcpy " & Sav & ", " & GA & ", " & Img (Arr_Desc_Size) & ", align 8");
                                 Emit_Store_Arr_Desc (Ctx, GA, PN & "_ptr", VInfo.First_Regs, VInfo.Last_Regs, TI.NDims);
                                 VInfo.Has_Desc    := True;
                                 VInfo.Desc_Global := To_Unbounded_String (GSym);
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                        Loc_Reg  => To_Unbounded_String (Sav),
                                                        Pir_Type => To_Unbounded_String ("@__ada_arr_desc"),
                                                        Align    => 8,
                                                        Ada_Type => CONST_VOID));
                              end;
                           elsif VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                              declare
                                 DR : constant String := Next_Reg (Ctx.E, "pdesc");
                                 FP : constant String := Next_Reg (Ctx.E, "pfp");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, DR & " = alloca @__ada_arr_desc, align 8");
                                 Emit_Store_Arr_Desc (Ctx, DR, PN & "_ptr", VInfo.First_Regs, VInfo.Last_Regs, TI.NDims);
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                   To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & DR & ", " & FP & ", align 8");
                                 VInfo.Has_Desc := True;
                              end;
                           end if;
                        elsif TI.Kind = TK_Record or else TI.Kind = TK_Constrained_Array then
                           VInfo.Kind     := VK_Param_Ptr;
                           VInfo.Pir_Name := To_Unbounded_String (PN);
                           if Needs_Constrained_Param (Ctx, PT, Mode) then
                              VInfo.Constrained_Reg := To_Unbounded_String (PN & "_con");
                           end if;
                           if Subp_Has_Stubs then
                              -- Export pointer display so separate subunits can access parent composite formal params
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "sppa");
                                 OldV : constant String := Next_Reg (Ctx.E, "sppo");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spps");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global ptr = zeroinit, align 8");
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca ptr, align 8");
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, OldV & " = load.ptr " & GA & ", align 8");
                                 Emit_Inst (Ctx.E, "store.ptr " & OldV & ", " & Sav & ", align 8");
                                 Emit_Inst (Ctx.E, "store.ptr " & PN & ", " & GA & ", align 8");
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                        Loc_Reg  => To_Unbounded_String (Sav),
                                                        Pir_Type => To_Unbounded_String ("ptr"),
                                                        Align    => 8,
                                                        Ada_Type => CONST_VOID));
                              end;
                           elsif VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                              declare
                                 FP : constant String := Next_Reg (Ctx.E, "pfp");
                              begin
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                   To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & PN & ", " & FP & ", align 8");
                              end;
                              if VInfo.Con_Captured_Slot > 0 and then Length (VInfo.Constrained_Reg) > 0 then
                                 declare
                                    CSp : constant String := Next_Reg (Ctx.E, "pcs");
                                    CFP : constant String := Next_Reg (Ctx.E, "pcf");
                                 begin
                                    Emit_Entry_Alloca (Ctx.E, CSp & " = alloca i8, align 1");
                                    Emit_Inst (Ctx.E, "store.i8 " & To_String (VInfo.Constrained_Reg) & ", " & CSp & ", align 1");
                                    Emit_Inst (Ctx.E,
                                      CFP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                      To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Con_Captured_Slot));
                                    Emit_Inst (Ctx.E, "store.ptr " & CSp & ", " & CFP & ", align 8");
                                 end;
                              end if;
                           end if;
                        elsif Mode = DN_OUT or else Mode = DN_IN_OUT then
                           -- Scalar OUT / IN OUT: Ada 83 LRM 6.2(7) copy-in / copy-out semantics!
                           if Subp_Has_Stubs then
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "spga");
                                 OldV : constant String := Next_Reg (Ctx.E, "spov");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spsv");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global " & To_String (TI.Pir_Type) & " = zeroinit, align " & Img (TI.Align));
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, OldV & " = load." & To_String (TI.Pir_Type) & " " & GA & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & OldV & ", " & Sav & ", align " & Img (TI.Align));
                                 if Mode = DN_IN_OUT then
                                    declare
                                       CI : constant String := Next_Reg (Ctx.E, "ci");
                                    begin
                                       Emit_Inst (Ctx.E, CI & " = load." & To_String (TI.Pir_Type) & " " & PN & ", align " & Img (TI.Align));
                                       Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & CI & ", " & GA & ", align " & Img (TI.Align));
                                    end;
                                 end if;
                                 VInfo.Kind     := VK_Global;
                                 VInfo.Pir_Name := To_Unbounded_String (GSym);
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (PN),
                                                        Loc_Reg  => To_Unbounded_String (GA),
                                                        Pir_Type => TI.Pir_Type,
                                                        Align    => TI.Align,
                                                        Ada_Type => PT));
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                        Loc_Reg  => To_Unbounded_String (Sav),
                                                        Pir_Type => TI.Pir_Type,
                                                        Align    => TI.Align,
                                                        Ada_Type => CONST_VOID));
                              end;
                           else
                              declare
                                 Loc : constant String := Next_Reg (Ctx.E, "io_" & Sanitize_Ident (Sym_Name (PID)) & "_");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, Loc & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 if Mode = DN_IN_OUT then
                                    declare
                                       CI : constant String := Next_Reg (Ctx.E, "ci");
                                    begin
                                       Emit_Inst (Ctx.E, CI & " = load." & To_String (TI.Pir_Type) & " " & PN & ", align " & Img (TI.Align));
                                       Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & CI & ", " & Loc & ", align " & Img (TI.Align));
                                    end;
                                 end if;
                                 if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                                    declare
                                       FP : constant String := Next_Reg (Ctx.E, "pfp");
                                    begin
                                       Emit_Inst (Ctx.E,
                                         FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                         To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                       Emit_Inst (Ctx.E, "store.ptr " & Loc & ", " & FP & ", align 8");
                                    end;
                                 end if;
                                 VInfo.Kind     := VK_Local_Alloca;
                                 VInfo.Pir_Name := To_Unbounded_String (Loc);
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (PN),
                                                        Loc_Reg  => To_Unbounded_String (Loc),
                                                        Pir_Type => TI.Pir_Type,
                                                        Align    => TI.Align,
                                                        Ada_Type => PT));
                              end;
                           end if;
                        else
                           -- Scalar IN parameter
                           if Subp_Has_Stubs then
                              declare
                                 GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Can_P);
                                 GA   : constant String := Next_Reg (Ctx.E, "spga");
                                 OldV : constant String := Next_Reg (Ctx.E, "spov");
                                 Sav  : constant String := Next_Reg (Ctx.E, "spsv");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym,
                                   "export " & GSym & " = global " & To_String (TI.Pir_Type) & " = zeroinit, align " & Img (TI.Align));
                                 Emit_Entry_Alloca (Ctx.E, Sav & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, OldV & " = load." & To_String (TI.Pir_Type) & " " & GA & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & OldV & ", " & Sav & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & PN & ", " & GA & ", align " & Img (TI.Align));
                                 VInfo.Kind     := VK_Global;
                                 VInfo.Pir_Name := To_Unbounded_String (GSym);
                                 Ctx.Copy_Outs.Append ((Ptr_Reg  => To_Unbounded_String (GA),
                                                        Loc_Reg  => To_Unbounded_String (Sav),
                                                        Pir_Type => TI.Pir_Type,
                                                        Align    => TI.Align,
                                                        Ada_Type => CONST_VOID));
                              end;
                           elsif VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                              -- Spill to local alloca so nested subprogram can read via pointer
                              declare
                                 Sp : constant String := Next_Reg (Ctx.E, "pspl");
                                 FP : constant String := Next_Reg (Ctx.E, "pfp");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, Sp & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " " & PN & ", " & Sp & ", align " & Img (TI.Align));
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                   To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & Sp & ", " & FP & ", align 8");
                                 VInfo.Kind     := VK_Local_Alloca;
                                 VInfo.Pir_Name := To_Unbounded_String (Sp);
                              end;
                           else
                              VInfo.Kind     := VK_Param_Val;
                              VInfo.Pir_Name := To_Unbounded_String (PN);
                           end if;
                        end if;
                        Ctx.Vars.Include (PK, VInfo);
                        Ctx.Vars.Include (Tree_Key (PID), VInfo);
                        Idx := Idx + 1;
                     end;
                  end loop;
               end;
            end loop;
         end;
      end if;
      end;

      -- Mark Secondary Stack at subprogram entry if it uses SS and does not return on SS
      declare
         Subp_SS_Mark : Unbounded_String;
      begin
         if not SI.Ret_Uncon_Arr and then Tree_Uses_SS (Ctx, Block_B) then
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", Subp_SS_Mark, Nounwind => True);
            Ctx.SS_Marks.Append (Subp_SS_Mark);
         end if;
      end;

      -- Emit subprogram block body
      Emit_Block_Body (Ctx, Block_B, Is_Subp => True);

      if not Is_Terminated (Ctx.E) then
         if SI.Is_Function then
            -- Reaching end of function without return raises Program_Error in Ada!
            Emit_SS_Release_To (Ctx, 0);
            Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
         else
            for CO of Ctx.Copy_Outs loop
               if To_String (CO.Pir_Type) = "@__ada_arr_desc" then
                  Emit_Inst (Ctx.E, "memcpy " & To_String (CO.Ptr_Reg) & ", " & To_String (CO.Loc_Reg) & ", " & Img (Arr_Desc_Size) & ", align 8");
               else
                  declare
                     V : constant String := Next_Reg (Ctx.E, "co");
                  begin
                     Emit_Inst (Ctx.E,
                       V & " = load." & To_String (CO.Pir_Type) & " " &
                       To_String (CO.Loc_Reg) & ", align " & Img (CO.Align));
                     Emit_Range_Check (Ctx, V, To_String (CO.Pir_Type), CO.Ada_Type);
                     Emit_Inst (Ctx.E,
                       "store." & To_String (CO.Pir_Type) & " " & V &
                       ", " & To_String (CO.Ptr_Reg) & ", align " & Img (CO.Align));
                  end;
               end if;
            end loop;
            Emit_SS_Release_To (Ctx, 0);
            Emit_Term (Ctx.E, "ret");
         end if;
      end if;

      End_Func (Ctx.E, Saved_Buf);

      -- Restore outer subprogram state
      Ctx.Cur_Subp_Key  := Saved_Key;
      Ctx.Cur_Subp_Node := Saved_Node;
      Ctx.Cur_Ret_Type  := Saved_Ret_T;
      Ctx.Cur_Ret_Comp  := Saved_Ret_Comp;
      Ctx.Cur_Ret_Uncon := Saved_Ret_Uncon;
      Ctx.Cur_Ret_Ptr   := Saved_Ret_Ptr;
      Ctx.Cur_Frame_Reg     := Saved_Frame;
      Ctx.Cur_SL_Reg        := Saved_SL;
      Ctx.Cur_Master_Reg    := Saved_Mst;
      Ctx.Cur_Task_Reg      := Saved_Tsk;
      Ctx.Cur_Task_Exit_Lbl := Saved_TExit;
      Ctx.Cur_Is_Func       := Saved_Is_Func;
      Ctx.Loops             := Saved_Loops;
      Ctx.Handlers          := Saved_Handlers;
      Ctx.Copy_Outs         := Saved_Copy_Outs;
      Ctx.SS_Marks                := Saved_SS_Marks;
      Ctx.SP_Marks                := Saved_SP_Marks;
      Ctx.Master_Stack            := Saved_Mst_Stack;
      Ctx.Accept_End_Lbls         := Saved_Acc_Lbls;
      Ctx.Accept_Mst_Depths       := Saved_Acc_Mst_Dep;
      Ctx.Cur_Task_Base_Mst_Depth := Saved_TBase_Mst;
      Ctx.Label_Mst_Depths        := Saved_Lbl_Mst_Dep;
      Ctx.Label_SS_Depths         := Saved_Lbl_SS_Dep;
      Ctx.Label_SP_Depths         := Saved_Lbl_SP_Dep;
      Ctx.Uplevel_Cache     := Saved_Uplevel;
   end Emit_Subprogram_Body;

   -- =========================================================================
   -- Top-Level Compilation Driver
   -- =========================================================================

   function Package_Has_Compiled_Body (PN : String; In_Unit_Bodies : String) return Boolean is
      Up_PN : constant String := Ada.Strings.Fixed.Translate (PN, Ada.Strings.Maps.Constants.Upper_Case_Map);
   begin
      if Ada.Strings.Fixed.Index (In_Unit_Bodies, "|" & Up_PN & "|") > 0 then
         return True;
      end if;
      -- Check ADA__LIB.CTL for a compiled .BDY file matching Up_PN
      if Ada.Directories.Exists ("ADA__LIB.CTL") then
         declare
            F    : Ada.Text_IO.File_Type;
            Line : String (1 .. 512);
            Last : Natural;
            Short_N : Unbounded_String;
         begin
            Ada.Text_IO.Open (F, Ada.Text_IO.In_File, "ADA__LIB.CTL");
            while not Ada.Text_IO.End_Of_File (F) loop
               Ada.Text_IO.Get_Line (F, Line, Last);
               if Last >= 3 and then Line (1 .. 2) = "U " then
                  Short_N := To_Unbounded_String (Ada.Strings.Fixed.Trim (Line (3 .. Last), Ada.Strings.Both));
                  if not Ada.Text_IO.End_Of_File (F) then
                     Ada.Text_IO.Get_Line (F, Line, Last);
                     declare
                        Full_N : constant String := Ada.Strings.Fixed.Trim (Line (1 .. Last), Ada.Strings.Both);
                     begin
                        if Full_N = Up_PN then
                           Ada.Text_IO.Close (F);
                           return Ada.Directories.Exists ("ADA__LIB/" & To_String (Short_N) & ".BDY");
                        end if;
                     end;
                  end if;
               end if;
            end loop;
            Ada.Text_IO.Close (F);
         exception
            when others =>
               if Ada.Text_IO.Is_Open (F) then Ada.Text_IO.Close (F); end if;
         end;
      end if;
      -- Direct 8-char fallback
      declare
         Short_8 : constant String :=
           (if Up_PN'Length <= 8 then Up_PN else Up_PN (Up_PN'First .. Up_PN'First + 7));
      begin
         return Ada.Directories.Exists ("ADA__LIB/" & Short_8 & ".BDY");
      end;
   end Package_Has_Compiled_Body;

   procedure Emit_Main_Wrapper
     (Ctx            : in out Context;
      Entry_Sym      : String;
      Comp_Node      : TREE;
      In_Unit_Bodies : String := "|")
   is
      Saved : Pir_Emit.Func_Buffer;
      Max_N : constant Natural := 128;
      type Elab_Node is record
         Name    : Unbounded_String; -- UPPERCASE unit name
         Is_Body : Boolean := False;
      end record;
      Nodes   : array (1 .. Max_N) of Elab_Node;
      N_Count : Natural := 0;
      -- Edge(U, V) = True means V must elaborate BEFORE U
      Edge    : array (1 .. Max_N, 1 .. Max_N) of Boolean := (others => (others => False));

      function Is_RTL_Pkg (PN : String) return Boolean is
      begin
         return PN = "_STANDRD" or else PN = "STANDARD" or else PN = "SYSTEM"
           or else PN = "TEXT_IO" or else PN = "IO_EXCEPTIONS" or else PN = "CALENDAR"
           or else PN = "SEQUENTIAL_IO" or else PN = "DIRECT_IO" or else PN = "LOW_LEVEL_IO"
           or else PN = "UNCHECKED_CONVERSION" or else PN = "UNCHECKED_DEALLOCATION";
      end Is_RTL_Pkg;

      function Is_Support_Pkg (PN : String) return Boolean is
      begin
         return PN = "REPORT" or else PN = "FCNDECL" or else PN = "SPPRT13";
      end Is_Support_Pkg;

      function Find_Or_Add (PN : String; Is_Bdy : Boolean) return Natural is
      begin
         for I in 1 .. N_Count loop
            if Nodes (I).Is_Body = Is_Bdy and then To_String (Nodes (I).Name) = PN then
               return I;
            end if;
         end loop;
         if N_Count < Max_N then
            N_Count := N_Count + 1;
            Nodes (N_Count) := (Name => To_Unbounded_String (PN), Is_Body => Is_Bdy);
            return N_Count;
         end if;
         return 0;
      end Find_Or_Add;

      function Find_Node (PN : String; Is_Bdy : Boolean) return Natural is
      begin
         for I in 1 .. N_Count loop
            if Nodes (I).Is_Body = Is_Bdy and then To_String (Nodes (I).Name) = PN then
               return I;
            end if;
         end loop;
         return 0;
      end Find_Node;

      function Root_Parent_Name (N : TREE) return String is
         Cur : TREE := N;
      begin
         for Step in 1 .. 16 loop
            exit when Is_Void_Tree (Cur);
            if KIND (Cur) = DN_SELECTED then
               Cur := D (AS_NAME, Cur);
            else
               return Ada.Strings.Fixed.Translate (Sym_Name (Cur), Ada.Strings.Maps.Constants.Upper_Case_Map);
            end if;
         end loop;
         return "";
      end Root_Parent_Name;

      procedure Register_Pkg (PN : String) is
         SI, BI : Natural;
      begin
         if PN'Length = 0 or else Is_RTL_Pkg (PN) then
            return;
         end if;
         SI := Find_Or_Add (PN, False);
         if Package_Has_Compiled_Body (PN, In_Unit_Bodies) then
            BI := Find_Or_Add (PN, True);
            if SI > 0 and then BI > 0 then
               Edge (BI, SI) := True;
            end if;
         end if;
      end Register_Pkg;

      procedure Collect_Withs_From_CU (CU : TREE) is
         WL : SEQ_TYPE := LIST (CU);
         TW : TREE;
      begin
         while not IS_EMPTY (WL) loop
            POP (WL, TW);
            declare
               WCU : constant TREE := D (TW_COMP_UNIT, TW);
               WAD : constant TREE := (if not Is_Void_Tree (WCU) then D (AS_ALL_DECL, WCU) else CONST_VOID);
            begin
               if not Is_Void_Tree (WAD) and then KIND (WAD) in DN_PACKAGE_DECL | DN_GENERIC_DECL then
                  declare
                     WPN : constant String :=
                       Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, WAD)), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  begin
                     Register_Pkg (WPN);
                  end;
               end if;
            end;
         end loop;
      end Collect_Withs_From_CU;

      procedure Add_Context_Edges (CU : TREE; U_Idx : Natural) is
         procedure Scan_List (Seq_Node : TREE) is
            CL : SEQ_TYPE;
            CE : TREE;
         begin
            if Is_Void_Tree (Seq_Node) then
               return;
            end if;
            CL := LIST (Seq_Node);
            while not IS_EMPTY (CL) loop
               POP (CL, CE);
               if KIND (CE) = DN_WITH then
                  declare
                     NL : SEQ_TYPE := LIST (D (AS_NAME_S, CE));
                     NM : TREE;
                  begin
                     while not IS_EMPTY (NL) loop
                        POP (NL, NM);
                        declare
                           WN : constant String :=
                             Ada.Strings.Fixed.Translate (Sym_Name (NM), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           VI : constant Natural := Find_Node (WN, False);
                        begin
                           if VI > 0 and then VI /= U_Idx then
                              Edge (U_Idx, VI) := True;
                           end if;
                        end;
                     end loop;
                  end;
                  Scan_List (D (AS_USE_PRAGMA_S, CE));
               elsif KIND (CE) = DN_PRAGMA then
                  declare
                     PN_Str : constant String :=
                       Ada.Strings.Fixed.Translate (Sym_Name (D (AS_USED_NAME_ID, CE)), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  begin
                     if PN_Str = "ELABORATE"
                       and then not Is_Void_Tree (D (AS_USED_NAME_ID, CE))
                       and then not Is_Void_Tree (D (SM_DEFN, D (AS_USED_NAME_ID, CE)))
                       and then not Is_Void_Tree (D (AS_GENERAL_ASSOC_S, CE))
                     then
                        declare
                           AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, CE));
                           AI : TREE;
                        begin
                           while not IS_EMPTY (AL) loop
                              POP (AL, AI);
                              declare
                                 Exp : constant TREE := (if KIND (AI) = DN_ASSOC then D (AS_EXP, AI) else AI);
                                 EN  : constant String :=
                                   Ada.Strings.Fixed.Translate (Sym_Name (Exp), Ada.Strings.Maps.Constants.Upper_Case_Map);
                                 VS  : constant Natural := Find_Node (EN, False);
                                 VB  : constant Natural := Find_Node (EN, True);
                              begin
                                 if VS > 0 and then VS /= U_Idx then Edge (U_Idx, VS) := True; end if;
                                 if VB > 0 and then VB /= U_Idx then Edge (U_Idx, VB) := True; end if;
                              end;
                           end loop;
                        end;
                     end if;
                  end;
               end if;
            end loop;
         end Scan_List;
      begin
         if U_Idx = 0 then
            return;
         end if;
         Scan_List (D (AS_CONTEXT_ELEM_S, CU));
      end Add_Context_Edges;

      L1 : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      CU : TREE;
   begin
      Begin_Func (Ctx.E, Saved, "export func @main() -> i32 cc ""c""");

      -- Pass 1: discover all non-RTL library packages across ALL CUs (specs, bodies, subunits, and their WITHs)
      while not IS_EMPTY (L1) loop
         POP (L1, CU);
         Collect_Withs_From_CU (CU);
         declare
            AD : constant TREE := D (AS_ALL_DECL, CU);
         begin
            if not Is_Void_Tree (AD) and then (KIND (AD) = DN_PACKAGE_DECL
              or else KIND (AD) = DN_GENERIC_DECL
              or else (KIND (AD) = DN_SUBPROG_ENTRY_DECL and then not Is_Void_Tree (D (AS_UNIT_KIND, AD))
                       and then KIND (D (AS_UNIT_KIND, AD)) = DN_INSTANTIATION))
            then
               Register_Pkg (Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, AD)), Ada.Strings.Maps.Constants.Upper_Case_Map));
            end if;
         end;
      end loop;

      -- Pass 2: add WITH and PRAGMA ELABORATE edges from every CU (spec, body, and subunit -> root parent body)
      L1 := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      while not IS_EMPTY (L1) loop
         POP (L1, CU);
         declare
            AD : constant TREE := D (AS_ALL_DECL, CU);
         begin
            if not Is_Void_Tree (AD) then
               if KIND (AD) = DN_PACKAGE_DECL or else KIND (AD) = DN_GENERIC_DECL
                 or else (KIND (AD) = DN_SUBPROG_ENTRY_DECL and then not Is_Void_Tree (D (AS_UNIT_KIND, AD))
                          and then KIND (D (AS_UNIT_KIND, AD)) = DN_INSTANTIATION)
               then
                  Add_Context_Edges (CU, Find_Node (Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, AD)), Ada.Strings.Maps.Constants.Upper_Case_Map), False));
               elsif KIND (AD) = DN_PACKAGE_BODY or else (KIND (AD) = DN_SUBPROGRAM_BODY and then KIND (Canonical_Def (D (AS_SOURCE_NAME, AD))) = DN_GENERIC_ID) then
                  Add_Context_Edges (CU, Find_Node (Ada.Strings.Fixed.Translate (Sym_Name (Canonical_Def (D (AS_SOURCE_NAME, AD))), Ada.Strings.Maps.Constants.Upper_Case_Map), True));
               elsif KIND (AD) = DN_SUBUNIT then
                  declare
                     Root_N : constant String := Root_Parent_Name (D (AS_NAME, AD));
                     BI     : constant Natural := Find_Node (Root_N, True);
                  begin
                     if BI > 0 then
                        Add_Context_Edges (CU, BI);
                     end if;
                  end;
               end if;
            end if;
         end;
      end loop;

      -- Support packages (REPORT, FCNDECL, SPPRT13) elaborate before user packages when unconstrained
      for U in 1 .. N_Count loop
         if not Is_Support_Pkg (To_String (Nodes (U).Name)) then
            for V in 1 .. N_Count loop
               if Is_Support_Pkg (To_String (Nodes (V).Name)) and then not Edge (V, U) then
                  Edge (U, V) := True;
               end if;
            end loop;
         end if;
      end loop;

      -- Topological sort (Kahn's: support first, specs before bodies, original order)
      declare
         Done   : array (1 .. Max_N) of Boolean := (others => False);
         Emitted : Natural := 0;
         Step_No : Natural := 0;
      begin
         while Emitted < N_Count loop
            declare
               Pick : Natural := 0;
               function Node_Prio (I : Natural) return Natural is
                  PN : constant String := To_String (Nodes (I).Name);
                  R  : Natural := (if PN = "REPORT" then 0 elsif Is_Support_Pkg (PN) then 10 else 20);
               begin
                  if Nodes (I).Is_Body then R := R + 5; end if;
                  return R;
               end Node_Prio;
            begin
               for I in 1 .. N_Count loop
                  if not Done (I) then
                     declare
                        Ready : Boolean := True;
                     begin
                        for J in 1 .. N_Count loop
                           if Edge (I, J) and then not Done (J) then
                              Ready := False;
                              exit;
                           end if;
                        end loop;
                        if Ready then
                           if Pick = 0 or else Node_Prio (I) < Node_Prio (Pick) then
                              Pick := I;
                           end if;
                        end if;
                     end;
                  end if;
               end loop;
               if Pick = 0 then
                  -- P3-5.15: Reject circular elaboration dependencies (LRM 10.5(2, 11))!
                  Ada.Text_IO.Put_Line (Ada.Text_IO.Standard_Error, "elaboration error: circular elaboration dependency detected");
                  raise Elab_Cycle_Error;
               end if;
               exit when Pick = 0;
               Done (Pick) := True;
               Emitted := Emitted + 1;
               Step_No := Step_No + 1;
               declare
                  PN_Low : constant String := Lower_Name (To_String (Nodes (Pick).Name));
                  Sym    : constant String := "@ada__" & PN_Low & (if Nodes (Pick).Is_Body then "__elab_body" else "__elab_spec");
                  NxtL   : constant String := ".el_" & Img (Step_No);
               begin
                  Add_Extern_Func (Ctx.E, Sym, "extern func " & Sym & "() -> void cc ""internal""");
                  Emit_Term (Ctx.E, "invoke " & Sym & "() then " & NxtL & " unwind .unhandled(exn)");
                  Emit_Label (Ctx.E, NxtL);
               end;
            end;
         end loop;
      end;

      -- Activate any library-level tasks before entering main, and wait for them on exit!
      Emit_Inst (Ctx.E, "%__gm = call @__ada_global_master()");
      Emit_Term (Ctx.E, "invoke @__ada_master_activate(%__gm) then .run_main unwind .unhandled(exn)");
      Emit_Label (Ctx.E, ".run_main");
      -- Invoke Ada main procedure with top-level unhandled-exception handler
      Emit_Term (Ctx.E, "invoke " & Entry_Sym & "() then .done unwind .unhandled(exn)");
      Emit_Label (Ctx.E, ".done");
      Emit_Inst (Ctx.E, "call @__ada_master_leave(%__gm)");
      Emit_Term (Ctx.E, "ret 0");
      Emit_Label (Ctx.E, ".unhandled", "%exn: ptr");
      Emit_Inst (Ctx.E, "call @__ada_unhandled_exception(%exn)");
      Emit_Term (Ctx.E, "unreachable");

      End_Func (Ctx.E, Saved);
   end Emit_Main_Wrapper;

   procedure Emit_Task_Body
     (Ctx       : in out Context;
      Body_Node : TREE)
   is
      TB_Id   : constant TREE := D (AS_SOURCE_NAME, Body_Node);
      TS      : constant TREE := (if not Is_Void_Tree (TB_Id) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TB_Id))) else CONST_VOID);
      Block_B : TREE := D (AS_BODY, Body_Node);
      TK      : Unsigned_32;
      TI      : Task_Info;

      Saved_Buf      : Pir_Emit.Func_Buffer;
      Saved_Key      : constant Unsigned_32 := Ctx.Cur_Subp_Key;
      Saved_Node     : constant TREE := Ctx.Cur_Subp_Node;
      Saved_Ret_T     : constant TREE := Ctx.Cur_Ret_Type;
      Saved_Ret_Comp  : constant Boolean := Ctx.Cur_Ret_Comp;
      Saved_Ret_Uncon : constant Boolean := Ctx.Cur_Ret_Uncon;
      Saved_Ret_Ptr   : constant Unbounded_String := Ctx.Cur_Ret_Ptr;
      Saved_Frame     : constant Unbounded_String := Ctx.Cur_Frame_Reg;
      Saved_SL        : constant Unbounded_String := Ctx.Cur_SL_Reg;
      Saved_Mst       : constant Unbounded_String := Ctx.Cur_Master_Reg;
      Saved_Tsk       : constant Unbounded_String := Ctx.Cur_Task_Reg;
      Saved_TExit     : constant Unbounded_String := Ctx.Cur_Task_Exit_Lbl;
      Saved_Is_Func   : constant Boolean := Ctx.Cur_Is_Func;
      Saved_Loops       : constant Loop_Vecs.Vector := Ctx.Loops;
      Saved_Handlers    : constant Handler_Vecs.Vector := Ctx.Handlers;
      Saved_Copy_Outs   : constant Copy_Out_Vecs.Vector := Ctx.Copy_Outs;
      Saved_SS_Marks    : constant Str_Vecs.Vector := Ctx.SS_Marks;
      Saved_SP_Marks    : constant Str_Vecs.Vector := Ctx.SP_Marks;
      Saved_Mst_Stack   : constant Str_Vecs.Vector := Ctx.Master_Stack;
      Saved_Acc_Lbls    : constant Str_Vecs.Vector := Ctx.Accept_End_Lbls;
      Saved_Acc_Mst_Dep : constant U32_Vecs.Vector := Ctx.Accept_Mst_Depths;
      Saved_TBase_Mst   : constant Natural := Ctx.Cur_Task_Base_Mst_Depth;
      Saved_Lbl_Mst_Dep : constant I64_Maps.Map := Ctx.Label_Mst_Depths;
      Saved_Lbl_SS_Dep  : constant I64_Maps.Map := Ctx.Label_SS_Depths;
      Saved_Lbl_SP_Dep  : constant I64_Maps.Map := Ctx.Label_SP_Depths;
      Saved_Uplevel     : constant Type_Maps.Map := Ctx.Uplevel_Cache;

      Captured_Vars  : Cap_Vecs.Vector;
      Nested_Subps   : U32_Vecs.Vector;
   begin
      if Is_Void_Tree (TS) or else KIND (TS) /= DN_TASK_SPEC then
         return;
      end if;
      Register_Task_Spec (Ctx, TS, Saved_Key);
      TK := Tree_Key (TS);
      TI := Ctx.Tasks.Element (TK);

      if Is_Void_Tree (Block_B) or else KIND (Block_B) = DN_STUB then
         declare
            Orig_TS : constant TREE :=
              (if not Is_Void_Tree (TB_Id)
               then Resolve_Type_Spec (D (SM_TYPE_SPEC, TB_Id))
               else CONST_VOID);
            Sub_Found : Boolean := False;
         begin
            if not Is_Void_Tree (Orig_TS) and then Ctx.Gen_Subunits.Contains (Tree_Key (Orig_TS)) then
               Block_B := D (AS_BODY, Ctx.Gen_Subunits.Element (Tree_Key (Orig_TS)));
               Sub_Found := True;
            elsif Ctx.Gen_Subunits.Contains (TK) then
               Block_B := D (AS_BODY, Ctx.Gen_Subunits.Element (TK));
               Sub_Found := True;
            end if;
            if not Sub_Found then
               Add_Extern_Func (Ctx.E, To_String (TI.Body_Fn),
                 "extern func " & To_String (TI.Body_Fn) & "(%__sl: ptr, %__self: ptr) -> void cc ""c""");
               return;
            end if;
         end;
      end if;

      declare
         TSI : Subp_Info;
      begin
         if Ctx.Subps.Contains (TK) then
            TSI := Ctx.Subps.Element (TK);
         end if;
         TSI.Pir_Name    := TI.Body_Fn;
         TSI.Parent_Subp := TI.Parent_Subp;
         TSI.Needs_SL    := True;
         Ctx.Subps.Include (TK, TSI);
      end;

      declare
         Temp_Owners : Var_Maps.Map;
         Local_Meta  : U32_Vecs.Vector;
         procedure Collect_Task_Owners (Node : TREE; In_Gen_Bdy : Boolean := False) is
            K : NODE_NAME;
         begin
            if Is_Void_Tree (Node) or else Node.TPG <= 0 or else Node.TLN = 0 then return; end if;
            K := KIND (Node);
            if K = DN_SUBPROGRAM_BODY or else K = DN_PACKAGE_BODY then
               declare
                  SN : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
               begin
                  if not Is_Void_Tree (SN) and then KIND (SN) = DN_GENERIC_ID then
                     if not Ctx.Generic_Body.Contains (Tree_Key (SN))
                       or else Is_Void_Tree (D (AS_BODY, Node))
                       or else KIND (D (AS_BODY, Node)) /= DN_STUB
                     then
                        Ctx.Generic_Body.Include (Tree_Key (SN), Node);
                     end if;
                  end if;
               end;
               if K = DN_SUBPROGRAM_BODY then return; end if;
               if K = DN_PACKAGE_BODY then
                  declare
                     Sub_P : constant TREE := Find_Gen_Subunit_Body (Ctx, Node);
                  begin
                     if not Is_Void_Tree (Sub_P) then
                        Collect_Task_Owners (D (AS_BODY, Sub_P), In_Gen_Bdy);
                        return;
                     end if;
                  end;
               end if;
            end if;
            if K not in CLASS_ALL_SOURCE or else K = DN_SUBPROGRAM_BODY or else K = DN_TASK_BODY then return; end if;
            if K = DN_TYPE_DECL then
               declare
                  TID : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS_A : constant TREE :=
                    (if not Is_Void_Tree (TID) and then KIND (TID) in CLASS_TYPE_NAME
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, TID))) else CONST_VOID);
               begin
                  if not Is_Void_Tree (TS_A) and then KIND (TS_A) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                    and then Type_Contains_Task (Ctx, D (SM_DESIG_TYPE, TS_A))
                  then
                     declare
                        BS : constant TREE := Base_Type_Spec (TS_A);
                     begin
                        if not Is_Void_Tree (BS) and then KIND (BS) = DN_ACCESS and then Is_Void_Tree (D (SM_DERIVED, BS)) then
                           Local_Meta.Append (Tree_Key (BS));
                        end if;
                     end;
                  end if;
               end;
               return;
            elsif K = DN_VARIABLE_DECL or else K = DN_CONSTANT_DECL then
               declare
                  NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Node));
                  ID : TREE;
               begin
                  while not IS_EMPTY (NS) loop
                     POP (NS, ID);
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := TK;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                     end;
                  end loop;
               end;
               return;
            elsif K = DN_TASK_DECL then
               declare
                  Src : constant TREE := D (AS_SOURCE_NAME, Node);
                  TS_M : TREE := CONST_VOID;
               begin
                  if not Is_Void_Tree (Src) and then KIND (Src) = DN_VARIABLE_ID then
                     declare
                        V : Var_Info;
                     begin
                        V.Owner_Subp    := TK;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (Canonical_Def (Src)), V);
                     end;
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, Src)));
                  elsif not Is_Void_Tree (Src) and then KIND (Src) in CLASS_TYPE_NAME then
                     TS_M := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Src)));
                  end if;
                  if not Is_Void_Tree (TS_M) and then KIND (TS_M) = DN_TASK_SPEC then
                     Local_Meta.Append (Tree_Key (TS_M));
                  end if;
               end;
               return;
            elsif K = DN_LENGTH_ENUM_REP then
               -- P3-6.6: Capture local dynamic 'STORAGE_SIZE slots in parent static-link frame
               declare
                  AN : constant TREE := D (AS_NAME, Node);
                  EN : constant TREE := D (AS_EXP, Node);
               begin
                  if not Is_Void_Tree (AN) and then KIND (AN) = DN_ATTRIBUTE and then not Is_Void_Tree (EN)
                    and then not Is_Void_Tree (D (AS_USED_NAME_ID, AN))
                    and then Sym_Name (D (AS_USED_NAME_ID, AN)) = "STORAGE_SIZE"
                  then
                     Local_Meta.Append (Tree_Key (EN));
                  end if;
               end;
               return;
            elsif K = DN_RENAMES_OBJ_DECL then
               declare
                  ID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := TK;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
               return;
            elsif K = DN_FOR or else K = DN_REVERSE then
               declare
                  ID : constant TREE := D (AS_SOURCE_NAME, Node);
                  V  : Var_Info;
               begin
                  V.Owner_Subp    := TK;
                  V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                  Temp_Owners.Include (Tree_Key (ID), V);
               end;
            elsif K = DN_SUBPROG_ENTRY_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     Collect_Task_Owners (D (SM_DECL_S, UK), In_Gen_Bdy);
                     return;
                  elsif not Is_Void_Tree (UK) and then KIND (UK) = DN_RENAMES_UNIT then
                     declare
                        SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        V   : Var_Info;
                     begin
                        V.Owner_Subp    := TK;
                        V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                        Temp_Owners.Include (Tree_Key (SID), V);
                     end;
                     return;
                  end if;
               end;
            elsif K = DN_PACKAGE_DECL then
               declare
                  UK : constant TREE := D (AS_UNIT_KIND, Node);
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     declare
                        PID    : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
                        Gen_Id : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, UK)));
                        GK     : constant Unsigned_32 := Tree_Key (Gen_Id);
                        GB     : TREE := CONST_VOID;
                     begin
                        Collect_Task_Owners (D (SM_DECL_S, UK), False);
                        Collect_Task_Owners (D (SM_SPEC, PID), False);
                        -- P3-5.1: Propagate nested generic bodies in instantiated package spec
                        declare
                           GSp : constant TREE := (if not Is_Void_Tree (Gen_Id) and then KIND (Gen_Id) = DN_GENERIC_ID then D (SM_SPEC, Gen_Id) else CONST_VOID);
                           ISp : constant TREE := D (SM_SPEC, PID);
                           Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
                        begin
                           if not Is_Void_Tree (GSp) and then KIND (GSp) = DN_PACKAGE_SPEC
                             and then not Is_Void_Tree (ISp) and then KIND (ISp) = DN_PACKAGE_SPEC
                           then
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S1, GSp), D (AS_DECL_S1, ISp));
                              Map_Decl_List_Ids (Ctx, D (AS_DECL_S2, GSp), D (AS_DECL_S2, ISp));
                           end if;
                           Ctx.Generic_Subst := Saved_Sub;
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
                              Collect_Task_Owners (D (AS_BODY, GB), True);
                           else
                              Collect_Task_Owners (GB, True);
                           end if;
                        end if;
                        return;
                     end;
                  end if;
               end;
            elsif K = DN_ACCEPT then
               declare
                  APS : constant TREE := D (AS_PARAM_S, Node);
               begin
                  if not Is_Void_Tree (APS) then
                     declare
                        PL : SEQ_TYPE := LIST (APS);
                        P  : TREE;
                     begin
                        while not IS_EMPTY (PL) loop
                           POP (PL, P);
                           declare
                              NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, P));
                              ID : TREE;
                           begin
                              while not IS_EMPTY (NS) loop
                                 POP (NS, ID);
                                 declare
                                    V : Var_Info;
                                 begin
                                    V.Owner_Subp    := TK;
                                    V.Captured_Slot := (if In_Gen_Bdy then 1 else 0);
                                    Temp_Owners.Include (Tree_Key (Canonical_Def (ID)), V);
                                    Temp_Owners.Include (Tree_Key (ID), V);
                                 end;
                              end loop;
                           end;
                        end loop;
                     end;
                  end if;
               end;
            end if;
            if ARITY (Node) = ARBITRARY then
               declare
                  L : SEQ_TYPE := LIST (Node);
                  It : TREE;
               begin
                  while not IS_EMPTY (L) loop POP (L, It); Collect_Task_Owners (It, In_Gen_Bdy); end loop;
               end;
            else
               for I in 1 .. ARITIES'Pos (ARITY (Node)) loop Collect_Task_Owners (DABS (I, Node), In_Gen_Bdy); end loop;
            end if;
         end Collect_Task_Owners;
      begin
         Collect_Task_Owners (Block_B);
         Scan_Tree_For_Captures (Ctx, Block_B, TK, Temp_Owners, Ctx.Subps, Captured_Vars, Nested_Subps, TK);
         if not Local_Meta.Is_Empty and then (not Nested_Subps.Is_Empty or else Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Block_B))) then
            for MK of Local_Meta loop
               declare
                  Found_M : Boolean := False;
               begin
                  for CP of Captured_Vars loop
                     if CP.Inst_Key = 0 and then CP.Var_Key = MK then
                        Found_M := True;
                        exit;
                     end if;
                  end loop;
                  if not Found_M then
                     Captured_Vars.Append ((Inst_Key => 0, Var_Key => MK));
                  end if;
               end;
            end loop;
         end if;
      end;

      declare
         TSI : Subp_Info := Ctx.Subps.Element (TK);
      begin
         for NK of Nested_Subps loop
            if Ctx.Subps.Contains (NK) then
               declare
                  NSI : Subp_Info := Ctx.Subps.Element (NK);
               begin
                  NSI.Needs_SL := True;
                  Ctx.Subps.Replace (NK, NSI);
               end;
            end if;
         end loop;
         if not Captured_Vars.Is_Empty or else not Nested_Subps.Is_Empty
           or else Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Block_B))
         then
            TSI.Has_Frame := True;
            TSI.Frame_Type := To_Unbounded_String ("@__frame_" & Tree_Key_Str (TS) & To_String (Ctx.Gen_Inst_Tag));
            declare
               -- Slot 0 = parent static link (%__sl), Slot 1 = this task's %__self, Slots 2.. = captured vars
               FT_Decl : Unbounded_String := To_Unbounded_String ("type " & To_String (TSI.Frame_Type) & " = { ptr, ptr");
            begin
               for Slot in 1 .. Natural (Captured_Vars.Length) loop
                  Append (FT_Decl, ", ptr");
                  declare
                     CP : constant Cap_Pair := Captured_Vars.Element (Slot - 1);
                     CK : constant Unsigned_32 := CP.Var_Key;
                  begin
                     if CP.Inst_Key /= 0 then
                        Ctx.Gen_Cap_Slots.Include
                          (Shift_Left (Unsigned_64 (CP.Inst_Key), 32) or Unsigned_64 (CK), Slot + 1);
                     else
                        declare
                           V : Var_Info;
                        begin
                           if Ctx.Vars.Contains (CK) then V := Ctx.Vars.Element (CK); end if;
                           V.Captured_Slot := Slot + 1;
                           V.Owner_Subp := TK;
                           Ctx.Vars.Include (CK, V);
                        end;
                     end if;
                  end;
               end loop;
               Append (FT_Decl, " }");
               Add_Type_Decl (Ctx.E, To_String (FT_Decl));
            end;
            Ctx.Subps.Replace (TK, TSI);
         end if;

         declare
            TS_Src : constant TREE := D (XD_SOURCE_NAME, TS);
            Link   : constant String :=
              (if Is_Library_Level (TB_Id)
                 or else (not Is_Void_Tree (TS_Src) and then Is_Library_Level (TS_Src))
                 or else Ctx.In_Subunit
               then "export " else "");
            Hdr  : constant String := Link & "func " & To_String (TI.Body_Fn) & "(%__sl: ptr, %__self: ptr) -> void cc ""c""";
         begin
            Begin_Func (Ctx.E, Saved_Buf, Hdr);
         end;

         Ctx.Cur_Subp_Key      := TK;
         Ctx.Cur_Subp_Node     := Body_Node;
         Ctx.Cur_Is_Func       := False;
         Ctx.Cur_Ret_Type      := CONST_VOID;
         Ctx.Cur_Ret_Comp      := False;
         Ctx.Cur_Ret_Uncon     := False;
         Ctx.Cur_Ret_Ptr       := Null_Unbounded_String;
         Ctx.Cur_SL_Reg        := To_Unbounded_String ("%__sl");
         Ctx.Cur_Frame_Reg     := Null_Unbounded_String;
         Ctx.Cur_Master_Reg    := Null_Unbounded_String;
         Ctx.Cur_Task_Reg      := To_Unbounded_String ("%__self");
         Ctx.Cur_Task_Exit_Lbl       := To_Unbounded_String (Next_Label (Ctx.E, "task_exit"));
         Ctx.Cur_Task_Base_Mst_Depth := 0;
         Ctx.Loops.Clear;
         Ctx.Handlers.Clear;
         Ctx.Copy_Outs.Clear;
         Ctx.SS_Marks.Clear;
         Ctx.SP_Marks.Clear;
         Ctx.Master_Stack.Clear;
         Ctx.Accept_End_Lbls.Clear;
         Ctx.Accept_Mst_Depths.Clear;
         Ctx.Label_Mst_Depths.Clear;
         Ctx.Label_SS_Depths.Clear;
         Ctx.Label_SP_Depths.Clear;
         Ctx.Uplevel_Cache.Clear;

         if TSI.Has_Frame then
            declare
               FR  : constant String := Next_Reg (Ctx.E, "tfr");
               FP  : constant String := Next_Reg (Ctx.E, "tfrp");
               FP1 : constant String := Next_Reg (Ctx.E, "tfrs");
            begin
               Emit_Entry_Alloca (Ctx.E, FR & " = alloca " & To_String (TSI.Frame_Type) & ", align 8");
               Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (TSI.Frame_Type) & ", " & FR & ", 0");
               Emit_Inst (Ctx.E, "store.ptr %__sl, " & FP & ", align 8");
               Emit_Inst (Ctx.E, FP1 & " = field_ptr " & To_String (TSI.Frame_Type) & ", " & FR & ", 1");
               Emit_Inst (Ctx.E, "store.ptr %__self, " & FP1 & ", align 8");
               Ctx.Cur_Frame_Reg := To_Unbounded_String (FR);
            end;
         end if;

         declare
            Item_S    : constant TREE := D (AS_ITEM_S, Block_B);
            Stm_S     : constant TREE := D (AS_STM_S, Block_B);
            Alt_S     : constant TREE := D (AS_ALTERNATIVE_S, Block_B);
            Has_Hdl   : constant Boolean := (not Is_Void_Tree (Alt_S)) and then (not IS_EMPTY (LIST (Alt_S)));
            Has_Tasks : constant Boolean := Item_List_Has_Tasks (Ctx, Item_S);
            Act_Fail  : constant String := Next_Label (Ctx.E, "tact_f");
            Top_Exc   : constant String := Next_Label (Ctx.E, "texc_top");
            Exn_Act   : constant String := Next_Reg (Ctx.E, "taex");
            Exn_Top   : constant String := Next_Reg (Ctx.E, "ttex");
            Local_Mst : Unbounded_String;
            Mst_Slot  : Unbounded_String;
            Has_Dyn   : Boolean := False;
            Dummy     : Unbounded_String;
         begin
            if Has_Tasks then
               declare
                  MS : constant String := Next_Reg (Ctx.E, "tmst");
               begin
                  Emit_Entry_Alloca (Ctx.E, MS & " = alloca ptr, align 8");
                  Emit_Inst (Ctx.E, "store.ptr 0, " & MS & ", align 8");
                  Mst_Slot := To_Unbounded_String (MS);
               end;
            end if;
            Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (Act_Fail),
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));
            if Has_Tasks then
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_enter", "", "ptr", Local_Mst);
               Emit_Inst (Ctx.E, "store.ptr " & To_String (Local_Mst) & ", " & To_String (Mst_Slot) & ", align 8");
               Ctx.Cur_Master_Reg := Local_Mst;
               Ctx.Master_Stack.Append (Local_Mst);
               Ctx.Cur_Task_Base_Mst_Depth := 1;
               Scan_Label_Master_Depths (Ctx, Stm_S, 1);
               Scan_Label_Master_Depths (Ctx, Alt_S, 1);
            else
               Ctx.Cur_Task_Base_Mst_Depth := 0;
               Scan_Label_Master_Depths (Ctx, Stm_S, 0);
               Scan_Label_Master_Depths (Ctx, Alt_S, 0);
            end if;
            -- P3-5.10 & P3-5.17: Export task-body declarations & exceptions when task body contains separate subunits!
            declare
               Task_Has_Stubs  : Boolean := False;
               Saved_Has_Stubs : constant Boolean := Ctx.Has_Stubs;
            begin
               if not Is_Void_Tree (Item_S) then
                  declare
                     SL_T : SEQ_TYPE := LIST (Item_S);
                     IT_T : TREE;
                  begin
                     while not IS_EMPTY (SL_T) loop
                        POP (SL_T, IT_T);
                        if (KIND (IT_T) = DN_SUBPROGRAM_BODY or else KIND (IT_T) = DN_PACKAGE_BODY or else KIND (IT_T) = DN_TASK_BODY)
                          and then not Is_Void_Tree (D (AS_BODY, IT_T))
                          and then KIND (D (AS_BODY, IT_T)) = DN_STUB
                          and then not Is_Gen_Subunit_Stub (Ctx, IT_T)
                        then
                           Task_Has_Stubs := True;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;
               if Task_Has_Stubs then
                  Ctx.Has_Stubs := True;
               end if;
               Emit_Decl_Items (Ctx, Item_S, Is_Global => Task_Has_Stubs, Export_All => Task_Has_Stubs, Has_Dyn => Has_Dyn);
               Ctx.Has_Stubs := Saved_Has_Stubs;
            end;
            Ctx.Handlers.Delete_Last;

            Emit_Call_Or_Invoke (Ctx, "@__ada_task_activated", "%__self", "", Dummy, Nounwind => True);

            Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (Top_Exc),
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => Null_Unbounded_String,
                                  SP_Mark       => Null_Unbounded_String));

            if Has_Hdl then
               declare
                  Hdl_Lbl   : constant String := Next_Label (Ctx.E, "exc_h");
                  After_Lbl : constant String := Next_Label (Ctx.E, "exc_end");
                  H_SP      : constant String := Next_Reg (Ctx.E, "thsp");
               begin
                  Emit_Inst (Ctx.E, H_SP & " = stacksave");
                  Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (Hdl_Lbl),
                                        Current_Exn   => Null_Unbounded_String,
                                        In_Handler    => False,
                                        SS_Mark       => Null_Unbounded_String,
                                        SP_Mark       => To_Unbounded_String (H_SP)));
                  if Has_Tasks then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate", To_String (Local_Mst), "", Dummy);
                  end if;
                  Emit_Stm_List (Ctx, Stm_S);
                  if not Is_Terminated (Ctx.E) then Emit_Term (Ctx.E, "jmp " & After_Lbl); end if;
                  Emit_Exception_Handlers (Ctx, Alt_S, Hdl_Lbl, After_Lbl);
                  Ctx.Handlers.Delete_Last;
                  Emit_Label (Ctx.E, After_Lbl);
               end;
            else
               if Has_Tasks then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate", To_String (Local_Mst), "", Dummy);
               end if;
               Emit_Stm_List (Ctx, Stm_S);
            end if;

            Ctx.Handlers.Delete_Last;
            if not Is_Terminated (Ctx.E) then
               Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));
            end if;

            Emit_Label (Ctx.E, Act_Fail, Exn_Act & ": ptr");
            -- Notify activator and set T'CALLABLE=FALSE first so we cannot deadlock with
            -- already-activated child tasks waiting on T or T's activator (LRM 9.3(5))
            Emit_Call_Or_Invoke (Ctx, "@__ada_task_activation_failed", "%__self", "", Dummy, Nounwind => True);
            if Has_Tasks then
               declare
                  ML : constant String := Next_Reg (Ctx.E, "tml");
               begin
                  Emit_Inst (Ctx.E, ML & " = load.ptr " & To_String (Mst_Slot) & ", align 8");
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_unactivated", ML, "", Dummy, Nounwind => True);
               end;
            end if;
            Emit_Term (Ctx.E, "ret");

            Emit_Label (Ctx.E, Top_Exc, Exn_Top & ": ptr");
            Emit_Term (Ctx.E, "jmp " & To_String (Ctx.Cur_Task_Exit_Lbl));

            Emit_Label (Ctx.E, To_String (Ctx.Cur_Task_Exit_Lbl));
            if Has_Tasks then
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_completed", "%__self", "", Dummy, Nounwind => True);
               Emit_Call_Or_Invoke (Ctx, "@__ada_master_leave", To_String (Local_Mst), "", Dummy);
            end if;
            Emit_Term (Ctx.E, "ret");
         end;

         End_Func (Ctx.E, Saved_Buf);

         Ctx.Cur_Subp_Key      := Saved_Key;
         Ctx.Cur_Subp_Node     := Saved_Node;
         Ctx.Cur_Ret_Type      := Saved_Ret_T;
         Ctx.Cur_Ret_Comp      := Saved_Ret_Comp;
         Ctx.Cur_Ret_Uncon     := Saved_Ret_Uncon;
         Ctx.Cur_Ret_Ptr       := Saved_Ret_Ptr;
         Ctx.Cur_Frame_Reg     := Saved_Frame;
         Ctx.Cur_SL_Reg        := Saved_SL;
         Ctx.Cur_Master_Reg    := Saved_Mst;
         Ctx.Cur_Task_Reg      := Saved_Tsk;
         Ctx.Cur_Task_Exit_Lbl := Saved_TExit;
         Ctx.Cur_Is_Func       := Saved_Is_Func;
         Ctx.Loops             := Saved_Loops;
         Ctx.Handlers          := Saved_Handlers;
         Ctx.Copy_Outs         := Saved_Copy_Outs;
         Ctx.SS_Marks                := Saved_SS_Marks;
         Ctx.SP_Marks                := Saved_SP_Marks;
         Ctx.Master_Stack            := Saved_Mst_Stack;
         Ctx.Accept_End_Lbls         := Saved_Acc_Lbls;
         Ctx.Accept_Mst_Depths       := Saved_Acc_Mst_Dep;
         Ctx.Cur_Task_Base_Mst_Depth := Saved_TBase_Mst;
         Ctx.Label_Mst_Depths        := Saved_Lbl_Mst_Dep;
         Ctx.Label_SS_Depths         := Saved_Lbl_SS_Dep;
         Ctx.Label_SP_Depths         := Saved_Lbl_SP_Dep;
         Ctx.Uplevel_Cache           := Saved_Uplevel;
      end;
   end Emit_Task_Body;

   procedure Emit_Compilation (Ctx : in out Context; Comp_Node : TREE; Only_CU : Natural := 0) is
      CU_List        : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      CU             : TREE;
      In_Unit_Bodies : Unbounded_String := To_Unbounded_String ("|");
   begin
      Ensure_Runtime_Externs (Ctx);

      -- Pre-scan compilation units in this file to record bodies, generics, latest CU per unit, and main CU
      declare
         Scan_L : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
         SCU    : TREE;
         CU_Idx : Natural := 0;
         Main_CU_Idx : Natural := 0;
         Max_U  : constant Natural := 128;
         type Unit_Latest is record
            Key_Str : Unbounded_String;
            Idx     : Natural := 0;
         end record;
         Latest     : array (1 .. Max_U) of Unit_Latest;
         Latest_Cnt : Natural := 0;
         procedure Set_Latest (KStr : String; Idx : Natural) is
         begin
            for I in 1 .. Latest_Cnt loop
               if To_String (Latest (I).Key_Str) = KStr then
                  Latest (I).Idx := Idx;
                  return;
               end if;
            end loop;
            if Latest_Cnt < Max_U then
               Latest_Cnt := Latest_Cnt + 1;
               Latest (Latest_Cnt) := (To_Unbounded_String (KStr), Idx);
            end if;
         end Set_Latest;
         function Get_Latest (KStr : String) return Natural is
         begin
            for I in 1 .. Latest_Cnt loop
               if To_String (Latest (I).Key_Str) = KStr then
                  return Latest (I).Idx;
               end if;
            end loop;
            return 0;
         end Get_Latest;
         function Subunit_Has_Generic_Ancestor (Name_Node : TREE) return Boolean is
            N : TREE := Name_Node;
         begin
            while not Is_Void_Tree (N) loop
               declare
                  Def : TREE := CONST_VOID;
               begin
                  if KIND (N) = DN_SELECTED then
                     Def := Canonical_Def (Get_Name_Defn (D (AS_DESIGNATOR, N)));
                     if not Is_Void_Tree (Def) and then KIND (Def) = DN_GENERIC_ID then
                        return True;
                     end if;
                     N := D (AS_NAME, N);
                  else
                     Def := Canonical_Def (Get_Name_Defn (N));
                     if not Is_Void_Tree (Def) and then KIND (Def) = DN_GENERIC_ID then
                        return True;
                     end if;
                     exit;
                  end if;
               end;
            end loop;
            return False;
         end Subunit_Has_Generic_Ancestor;

         procedure Scan_Nested_Generics (Item_S : TREE) is
            IL : SEQ_TYPE;
            It : TREE;
         begin
            if Is_Void_Tree (Item_S) then
               return;
            end if;
            IL := LIST (Item_S);
            while not IS_EMPTY (IL) loop
               POP (IL, It);
               if KIND (It) = DN_GENERIC_DECL then
                  declare
                     GID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
                  begin
                     if not Is_Void_Tree (GID) then
                        Ctx.Generic_Decls.Include (Tree_Key (GID), It);
                     end if;
                  end;
               elsif KIND (It) = DN_PACKAGE_DECL then
                  declare
                     Hdr : constant TREE := D (AS_HEADER, It);
                  begin
                     if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                        Scan_Nested_Generics (D (AS_DECL_S1, Hdr));
                        Scan_Nested_Generics (D (AS_DECL_S2, Hdr));
                     end if;
                  end;
               elsif KIND (It) = DN_PACKAGE_BODY then
                  declare
                     NPID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
                     NBdy : constant TREE := D (AS_BODY, It);
                  begin
                     if not Is_Void_Tree (NPID) and then KIND (NPID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (NPID))
                          or else Is_Void_Tree (NBdy)
                          or else KIND (NBdy) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (NPID), It);
                        end if;
                     elsif not Is_Void_Tree (NBdy) and then KIND (NBdy) = DN_BLOCK_BODY then
                        Scan_Nested_Generics (D (AS_ITEM_S, NBdy));
                     end if;
                  end;
               elsif KIND (It) = DN_SUBPROGRAM_BODY then
                  declare
                     NSID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, It));
                     NBdy : constant TREE := D (AS_BODY, It);
                  begin
                     if not Is_Void_Tree (NSID) and then KIND (NSID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (NSID))
                          or else Is_Void_Tree (NBdy)
                          or else KIND (NBdy) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (NSID), It);
                        end if;
                     end if;
                  end;
               end if;
            end loop;
         end Scan_Nested_Generics;
      begin
         while not IS_EMPTY (Scan_L) loop
            POP (Scan_L, SCU);
            CU_Idx := CU_Idx + 1;
            declare
               Raw_AD   : constant TREE := D (AS_ALL_DECL, SCU);
               Was_Subu : constant Boolean := (KIND (Raw_AD) = DN_SUBUNIT);
               AD       : constant TREE := (if Was_Subu then D (AS_SUBUNIT_BODY, Raw_AD) else Raw_AD);
            begin
               if Was_Subu and then Subunit_Has_Generic_Ancestor (D (AS_NAME, Raw_AD)) then
                  if KIND (AD) = DN_TASK_BODY then
                     declare
                        T_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                        T_Sp : TREE := CONST_VOID;
                     begin
                        if not Is_Void_Tree (T_Id) then
                           if KIND (T_Id) = DN_TASK_BODY_ID then T_Sp := Resolve_Type_Spec (D (SM_TYPE_SPEC, T_Id));
                           elsif KIND (T_Id) = DN_VARIABLE_ID then T_Sp := Resolve_Type_Spec (D (SM_OBJ_TYPE, T_Id));
                           elsif KIND (T_Id) in CLASS_TYPE_NAME then T_Sp := Resolve_Type_Spec (D (SM_TYPE_SPEC, T_Id));
                           end if;
                           if not Is_Void_Tree (T_Sp) then
                              Ctx.Gen_Subunits.Include (Tree_Key (T_Sp), AD);
                           end if;
                        end if;
                     end;
                  elsif KIND (AD) in DN_SUBPROGRAM_BODY | DN_PACKAGE_BODY then
                     declare
                        S_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     begin
                        if not Is_Void_Tree (S_Id) then
                           Ctx.Gen_Subunits.Include (Tree_Key (S_Id), AD);
                        end if;
                     end;
                  end if;
               end if;
               if KIND (AD) = DN_PACKAGE_DECL and then not Was_Subu then
                  Set_Latest ("PS:" & Ada.Strings.Fixed.Translate (Sym_Name (D (AS_SOURCE_NAME, AD)), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                  if not Is_Void_Tree (D (AS_HEADER, AD)) and then KIND (D (AS_HEADER, AD)) = DN_PACKAGE_SPEC then
                     Scan_Nested_Generics (D (AS_DECL_S1, D (AS_HEADER, AD)));
                     Scan_Nested_Generics (D (AS_DECL_S2, D (AS_HEADER, AD)));
                  end if;
               elsif KIND (AD) = DN_PACKAGE_BODY then
                  declare
                     PID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     PBdy : constant TREE := D (AS_BODY, AD);
                  begin
                     if KIND (PID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (PID))
                          or else Is_Void_Tree (PBdy)
                          or else KIND (PBdy) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (PID), AD);
                        end if;
                        if not Was_Subu then
                           declare
                              UPN : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           begin
                              Set_Latest ("PB:" & UPN, CU_Idx);
                              Append (In_Unit_Bodies, UPN & "|");
                           end;
                        end if;
                     else
                        if not Is_Void_Tree (PBdy) and then KIND (PBdy) = DN_BLOCK_BODY then
                           Scan_Nested_Generics (D (AS_ITEM_S, PBdy));
                        end if;
                        if not Was_Subu then
                           declare
                              UPN : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           begin
                              Set_Latest ("PB:" & UPN, CU_Idx);
                              Append (In_Unit_Bodies, UPN & "|");
                           end;
                        end if;
                     end if;
                  end;
               elsif KIND (AD) = DN_GENERIC_DECL then
                  declare
                     GID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                  begin
                     Ctx.Generic_Decls.Include (Tree_Key (GID), AD);
                     if not Was_Subu then
                        Set_Latest ("PS:" & Ada.Strings.Fixed.Translate (Sym_Name (GID), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                     end if;
                  end;
               elsif KIND (AD) = DN_SUBPROGRAM_BODY then
                  declare
                     SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     Hdr : constant TREE := D (AS_HEADER, AD);
                  begin
                     if KIND (SID) = DN_GENERIC_ID then
                        if not Ctx.Generic_Body.Contains (Tree_Key (SID))
                          or else Is_Void_Tree (D (AS_BODY, AD))
                          or else KIND (D (AS_BODY, AD)) /= DN_STUB
                        then
                           Ctx.Generic_Body.Include (Tree_Key (SID), AD);
                        end if;
                        if not Was_Subu then
                           declare
                              UPN : constant String := Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                           begin
                              Set_Latest ("PB:" & UPN, CU_Idx);
                              Append (In_Unit_Bodies, UPN & "|");
                           end;
                        end if;
                     elsif not Was_Subu then
                        Set_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                        if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PROCEDURE_SPEC
                          and then (Is_Void_Tree (D (AS_PARAM_S, Hdr)) or else IS_EMPTY (LIST (D (AS_PARAM_S, Hdr))))
                        then
                           Main_CU_Idx := CU_Idx;
                        end if;
                     end if;
                  end;
               elsif KIND (AD) = DN_SUBPROG_ENTRY_DECL and then not Was_Subu
                 and then not Is_Void_Tree (D (AS_UNIT_KIND, AD))
                 and then KIND (D (AS_UNIT_KIND, AD)) = DN_INSTANTIATION
               then
                  declare
                     SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, AD));
                     Sp  : constant TREE := (if not Is_Void_Tree (SID) then D (SM_SPEC, SID) else CONST_VOID);
                  begin
                     Set_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map), CU_Idx);
                     if not Is_Void_Tree (Sp) and then KIND (Sp) = DN_PROCEDURE_SPEC
                       and then (Is_Void_Tree (D (AS_PARAM_S, Sp)) or else IS_EMPTY (LIST (D (AS_PARAM_S, Sp))))
                     then
                        Main_CU_Idx := CU_Idx;
                     end if;
                  end;
               end if;
            end;
         end loop;

      CU_Idx := 0;
      while not IS_EMPTY (CU_List) loop
         POP (CU_List, CU);
         CU_Idx := CU_Idx + 1;
         if Only_CU = 0 or else CU_Idx = Only_CU then
         declare
            Raw_All  : constant TREE := D (AS_ALL_DECL, CU);
            All_Decl : TREE := Raw_All;
            K        : NODE_NAME := KIND (All_Decl);
            Is_Subu  : constant Boolean := (K = DN_SUBUNIT);
            function Subunit_Parent_Prefix (N : TREE) return String is
            begin
               if Is_Void_Tree (N) then return ""; end if;
               if KIND (N) = DN_SELECTED then
                  return Subunit_Parent_Prefix (D (AS_NAME, N)) & "__" & Sanitize_Ident (Sym_Name (D (AS_DESIGNATOR, N)));
               else
                  return Sanitize_Ident (Sym_Name (N));
               end if;
            end Subunit_Parent_Prefix;
         begin
            Ctx.In_Subunit := Is_Subu;
            if K = DN_SUBUNIT then
               if Subunit_Has_Generic_Ancestor (D (AS_NAME, Raw_All)) then
                  All_Decl := CONST_VOID;
                  K := DN_VOID;
               else
                  All_Decl := D (AS_SUBUNIT_BODY, All_Decl);
                  K := KIND (All_Decl);
               end if;
            end if;
            if K = DN_TASK_BODY then
               Emit_Task_Body (Ctx, All_Decl);
            elsif K = DN_GENERIC_DECL then
               declare
                  GID   : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                  UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (GID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  PName : constant String := Lower_Name (Sym_Name (GID));
                  GSym  : constant String := "@ada_gelab__" & Qualified_Ada_Name (GID);
                  GTI   : Type_Info;
                  Saved : Pir_Emit.Func_Buffer;
               begin
                  if not Is_Subu and then Get_Latest ("PS:" & UPN) = CU_Idx then
                     Add_Global_Decl (Ctx.E, GSym, "export " & GSym & " = global i8 = 0, align 1");
                     GTI.Pir_Type := To_Unbounded_String (GSym);
                     Ctx.Gen_Elab_Flags.Include (Tree_Key (GID), GTI);
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_spec() -> void cc ""internal""");
                     declare
                        GA : constant String := Next_Reg (Ctx.E, "lgea");
                     begin
                        Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                        Emit_Inst (Ctx.E, "store.i8 0, " & GA & ", align 1");
                     end;
                     Emit_Term (Ctx.E, "ret");
                     End_Func (Ctx.E, Saved);
                  end if;
               end;

            elsif K = DN_SUBPROGRAM_BODY then
               declare
                  SID        : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                  Is_Subunit : constant Boolean := Is_Subu;
               begin
                  if KIND (SID) = DN_GENERIC_ID then
                     declare
                        UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                        PName : constant String := Lower_Name (Sym_Name (SID));
                        GSym  : constant String := "@ada_gelab__" & Qualified_Ada_Name (SID);
                        Saved : Pir_Emit.Func_Buffer;
                     begin
                        if not Is_Subunit and then Get_Latest ("PB:" & UPN) = CU_Idx then
                           if not Ctx.Gen_Elab_Flags.Contains (Tree_Key (SID)) then
                              Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global i8, align 1");
                           end if;
                           Begin_Func (Ctx.E, Saved,
                             "export func @ada__" & PName & "__elab_body() -> void cc ""internal""");
                           declare
                              GA : constant String := Next_Reg (Ctx.E, "lge1");
                           begin
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                           end;
                           Emit_Term (Ctx.E, "ret");
                           End_Func (Ctx.E, Saved);
                        end if;
                     end;
                  else
                     if Is_Subunit or else Get_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map)) = CU_Idx then
                        Emit_Subprogram_Body (Ctx, All_Decl, Is_Exported => True, Is_Subunit => Is_Subunit);
                        if Ctx.Emit_Main and then not Is_Subunit and then CU_Idx = Main_CU_Idx then
                           declare
                              SI : constant Subp_Info := Ctx.Subps.Element (Tree_Key (SID));
                           begin
                              Emit_Main_Wrapper (Ctx, To_String (SI.Pir_Name), Comp_Node, To_String (In_Unit_Bodies));
                           end;
                        end if;
                     end if;
                  end if;
               end;

            elsif K = DN_SUBPROG_ENTRY_DECL then
               declare
                  SID   : constant TREE := D (AS_SOURCE_NAME, All_Decl);
                  PName : constant String := Lower_Name (Sym_Name (SID));
                  UK    : constant TREE := D (AS_UNIT_KIND, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
               begin
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION
                    and then (Is_Subu or else Get_Latest ("SB:" & Ada.Strings.Fixed.Translate (Sym_Name (SID), Ada.Strings.Maps.Constants.Upper_Case_Map)) = CU_Idx)
                  then
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_spec() -> void cc ""internal""");
                     Emit_Generic_Instantiation (Ctx, All_Decl, Export_All => True);
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "ret");
                     end if;
                     End_Func (Ctx.E, Saved);
                     if Ctx.Emit_Main and then not Is_Subu and then CU_Idx = Main_CU_Idx then
                        Emit_Main_Wrapper (Ctx, "@ada__" & PName, Comp_Node, To_String (In_Unit_Bodies));
                     end if;
                  end if;
               end;

            elsif K = DN_PACKAGE_DECL then
               declare
                  PID   : constant TREE := D (AS_SOURCE_NAME, All_Decl);
                  UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  PName : constant String := Lower_Name (Sym_Name (PID));
                  UK    : constant TREE := D (AS_UNIT_KIND, All_Decl);
                  Hdr   : constant TREE := D (AS_HEADER, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
                  Dyn1, Dyn2 : Boolean;
               begin
                  if Is_Subu or else Get_Latest ("PS:" & UPN) = CU_Idx then
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_spec() -> void cc ""internal""");
                     if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                        Emit_Generic_Instantiation (Ctx, All_Decl, Export_All => True);
                     elsif not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                        declare
                           Has_Body   : constant Boolean := Package_Has_Compiled_Body (UPN, To_String (In_Unit_Bodies));
                           Spec_Has_T : constant Boolean :=
                             Item_List_Has_Tasks (Ctx, D (AS_DECL_S1, Hdr))
                             or else Item_List_Has_Tasks (Ctx, D (AS_DECL_S2, Hdr));
                           Saved_G    : constant Integer_64 := Ctx.Cur_Task_Group_Id;
                           Pkg_Gid    : constant Integer_64 := Integer_64 (Lib_Binder.Lib_Pkg_Task_Group_Id (UPN));
                           GM_R, Dummy : Unbounded_String;
                        begin
                           if Spec_Has_T then
                              Ctx.Cur_Task_Group_Id := Pkg_Gid;
                           end if;
                           Emit_Decl_Items (Ctx, D (AS_DECL_S1, Hdr), Is_Global => True, Export_All => True, Has_Dyn => Dyn1);
                           Emit_Decl_Items (Ctx, D (AS_DECL_S2, Hdr), Is_Global => True, Export_All => True, Has_Dyn => Dyn2);
                           if Spec_Has_T then
                              Ctx.Cur_Task_Group_Id := Saved_G;
                              -- In single-file mode without external binder, activate bodyless spec tasks right away
                              if Ctx.Emit_Main and then not Has_Body then
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM_R, Nounwind => True);
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_group",
                                   To_String (GM_R) & ", " & Img (Long_Long_Integer (Pkg_Gid)), "", Dummy);
                              end if;
                           end if;
                        end;
                     end if;
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "ret");
                     end if;
                     End_Func (Ctx.E, Saved);
                  end if;
               end;

            elsif K = DN_PACKAGE_BODY then
               declare
                  PID   : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                  UPN   : constant String := Ada.Strings.Fixed.Translate (Sym_Name (PID), Ada.Strings.Maps.Constants.Upper_Case_Map);
                  Sub_P : constant String := (if Is_Subu then Subunit_Parent_Prefix (D (AS_NAME, Raw_All)) else "");
                  Q_PN  : constant String := Qualified_Ada_Name (PID);
                  PName : constant String :=
                    (if not Is_Subu then Lower_Name (Sym_Name (PID))
                     elsif Ada.Strings.Fixed.Index (Q_PN, "__") > 0 then Q_PN
                     elsif Sub_P'Length > 0 then Sub_P & "__" & Sanitize_Ident (Sym_Name (PID))
                     else Q_PN);
                  Bdy   : constant TREE := D (AS_BODY, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
                  Dyn   : Boolean;
                  Is_Sup : constant Boolean :=
                    UPN = "REPORT" or else UPN = "FCNDECL" or else UPN = "SPPRT13" or else UPN = "CHECKFILE";
               begin
                  if KIND (PID) = DN_GENERIC_ID then
                     if not Is_Subu and then Get_Latest ("PB:" & UPN) = CU_Idx then
                        declare
                           GSym : constant String := "@ada_gelab__" & Qualified_Ada_Name (PID);
                        begin
                           if not Ctx.Gen_Elab_Flags.Contains (Tree_Key (PID)) then
                              Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global i8, align 1");
                           end if;
                           Begin_Func (Ctx.E, Saved,
                             "export func @ada__" & PName & "__elab_body() -> void cc ""internal""");
                           declare
                              GA : constant String := Next_Reg (Ctx.E, "lge1");
                           begin
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Inst (Ctx.E, "store.i8 1, " & GA & ", align 1");
                           end;
                           Emit_Term (Ctx.E, "ret");
                           End_Func (Ctx.E, Saved);
                        end;
                     end if;
                  elsif Is_Subu or else Get_Latest ("PB:" & UPN) = CU_Idx then
                     Begin_Func (Ctx.E, Saved,
                       "export func @ada__" & PName & "__elab_body() -> void cc ""internal""");
                     if not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_BLOCK_BODY then
                        declare
                           Spec_Hdr   : constant TREE :=
                             (if KIND (PID) = DN_PACKAGE_ID then D (SM_SPEC, PID) else CONST_VOID);
                           Spec_Has_T : constant Boolean :=
                             not Is_Void_Tree (Spec_Hdr) and then KIND (Spec_Hdr) = DN_PACKAGE_SPEC
                             and then (Item_List_Has_Tasks (Ctx, D (AS_DECL_S1, Spec_Hdr))
                                       or else Item_List_Has_Tasks (Ctx, D (AS_DECL_S2, Spec_Hdr)));
                           Bdy_Has_T  : constant Boolean := Item_List_Has_Tasks (Ctx, D (AS_ITEM_S, Bdy));
                           Any_Has_T  : constant Boolean := Spec_Has_T or else Bdy_Has_T;
                           Saved_G    : constant Integer_64 := Ctx.Cur_Task_Group_Id;
                           Gid_Name   : constant String :=
                             (if Is_Subu then Ada.Strings.Fixed.Translate (PName, Ada.Strings.Maps.Constants.Upper_Case_Map)
                              else UPN);
                           Pkg_Gid    : constant Integer_64 := Integer_64 (Lib_Binder.Lib_Pkg_Task_Group_Id (Gid_Name));
                           GM_R, Dummy : Unbounded_String;
                           Saved_Mst   : constant Unbounded_String := Ctx.Cur_Master_Reg;
                           Subu_Mst    : Unbounded_String;
                           Prev_LPB : constant Boolean := Ctx.In_Lib_Pkg_Body;
                           Alt_S    : constant TREE := D (AS_ALTERNATIVE_S, Bdy);
                           Has_Hdl  : constant Boolean := not Is_Void_Tree (Alt_S) and then not IS_EMPTY (LIST (Alt_S));
                           Hdl_Lbl, After_Lbl : Unbounded_String;
                        begin
                           -- P3-4.9: Separate local package body subunit attaches & activates tasks on enclosing active master!
                           if Is_Subu then
                              declare
                                 AM_R  : Unbounded_String;
                                 GM_F  : Unbounded_String;
                                 Is_N  : constant String := Next_Reg (Ctx.E, "samn");
                                 Sel_M : constant String := Next_Reg (Ctx.E, "samm");
                              begin
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_get_active_master", "", "ptr", AM_R, Nounwind => True);
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM_F, Nounwind => True);
                                 Emit_Inst (Ctx.E, Is_N & " = cmp_eq.ptr " & To_String (AM_R) & ", 0");
                                 Emit_Inst (Ctx.E, Sel_M & " = select.ptr " & Is_N & ", " & To_String (GM_F) & ", " & To_String (AM_R));
                                 Subu_Mst := To_Unbounded_String (Sel_M);
                                 Ctx.Cur_Master_Reg := Subu_Mst;
                              end;
                           end if;
                           if Any_Has_T then
                              Ctx.Cur_Task_Group_Id := Pkg_Gid;
                           end if;
                           Ctx.In_Lib_Pkg_Body := (not Is_Subu and then not Is_Sup);
                           Emit_Decl_Items (Ctx, D (AS_ITEM_S, Bdy), Is_Global => True, Export_All => True, Has_Dyn => Dyn);
                           Ctx.In_Lib_Pkg_Body := Prev_LPB;
                           if Any_Has_T then
                              Ctx.Cur_Task_Group_Id := Saved_G;
                           end if;
                           if Has_Hdl then
                              Hdl_Lbl   := To_Unbounded_String (Next_Label (Ctx.E, "lpexc_h"));
                              After_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "lpexc_end"));
                              Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                                    Current_Exn   => Null_Unbounded_String,
                                                    In_Handler    => False,
                                                    SS_Mark       => Null_Unbounded_String,
                                                    SP_Mark       => Null_Unbounded_String));
                           end if;
                           if Any_Has_T then
                              if Is_Subu and then Length (Subu_Mst) > 0 then
                                 GM_R := Subu_Mst;
                              else
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM_R, Nounwind => True);
                              end if;
                              Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_group",
                                To_String (GM_R) & ", " & Img (Long_Long_Integer (Pkg_Gid)), "", Dummy);
                           end if;
                           Scan_Label_Master_Depths (Ctx, D (AS_STM_S, Bdy),
                             Natural (Ctx.Master_Stack.Length), Natural (Ctx.SS_Marks.Length), Natural (Ctx.SP_Marks.Length));
                           Scan_Label_Master_Depths (Ctx, Alt_S,
                             Natural (Ctx.Master_Stack.Length), Natural (Ctx.SS_Marks.Length), Natural (Ctx.SP_Marks.Length));
                           Emit_Stm_List (Ctx, D (AS_STM_S, Bdy));
                           if Has_Hdl then
                              if not Is_Terminated (Ctx.E) then
                                 Emit_Term (Ctx.E, "jmp " & To_String (After_Lbl));
                              end if;
                              Emit_Exception_Handlers (Ctx, Alt_S, To_String (Hdl_Lbl), To_String (After_Lbl));
                              Ctx.Handlers.Delete_Last;
                              Emit_Label (Ctx.E, To_String (After_Lbl));
                           end if;
                           Ctx.Cur_Master_Reg := Saved_Mst;
                        end;
                     end if;
                     if not Is_Terminated (Ctx.E) then
                        Emit_Term (Ctx.E, "ret");
                     end if;
                     End_Func (Ctx.E, Saved);
                  end if;
               end;
            end if;
         end;
         end if;
      end loop;
      Ctx.In_Subunit := False;
      end;
   end Emit_Compilation;

end Cg_Stmt;
