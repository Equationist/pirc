-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces; use Interfaces;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr; use Cg_Expr;

package body Cg_Stmt is

   -- Forward declarations
   procedure Emit_Subprogram_Body
     (Ctx          : in out Context;
      Body_Node    : TREE;
      Override_Id  : TREE := CONST_VOID;
      Override_Spec : TREE := CONST_VOID;
      Is_Exported  : Boolean := False);

   procedure Emit_Decl_Items
     (Ctx        : in out Context;
      Item_S     : TREE;
      Is_Global  : Boolean;
      Export_All : Boolean;
      Has_Dyn    : out Boolean);

   procedure Emit_Block_Body
     (Ctx         : in out Context;
      Block_Node  : TREE;
      Is_Subp     : Boolean);

   procedure Emit_Stm_List (Ctx : in out Context; Stm_S : TREE);
   procedure Emit_Stm (Ctx : in out Context; Stm : TREE);

   -- =========================================================================
   -- Static-Link Capture Pre-Scan
   -- =========================================================================
   -- Walks nested subprograms inside Owner_Key to find any references to
   -- variables owned by Owner_Key (or ancestors), populating Captured_Vars.

   procedure Scan_Tree_For_Captures
     (Node          : TREE;
      Current_Subp  : Unsigned_32;
      Owner_Map     : Var_Maps.Map;
      Parent_Map    : in out Subp_Maps.Map;
      Captured_Vars : in out U32_Vecs.Vector;
      Target_Owner  : Unsigned_32)
   is
      K : NODE_NAME;
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
            Def : constant TREE := Canonical_Def (D (SM_DEFN, Node));
            DK  : constant Unsigned_32 := Tree_Key (Def);
         begin
            if Current_Subp /= Target_Owner and then Owner_Map.Contains (DK) then
               declare
                  V : constant Var_Info := Owner_Map.Element (DK);
               begin
                  if V.Owner_Subp = Target_Owner then
                     -- Record captured variable if not already listed
                     declare
                        Found : Boolean := False;
                     begin
                        for E of Captured_Vars loop
                           if E = DK then Found := True; exit; end if;
                        end loop;
                        if not Found then
                           Captured_Vars.Append (DK);
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

      if K = DN_SUBPROGRAM_BODY then
         declare
            Subp_Id : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Node));
            SK      : constant Unsigned_32 := Tree_Key (Subp_Id);
         begin
            if SK /= Current_Subp then
               if not Parent_Map.Contains (SK) then
                  declare
                     SI : Subp_Info;
                  begin
                     SI.Parent_Subp := Current_Subp;
                     Parent_Map.Insert (SK, SI);
                  end;
               else
                  declare
                     SI : Subp_Info := Parent_Map.Element (SK);
                  begin
                     SI.Parent_Subp := Current_Subp;
                     Parent_Map.Replace (SK, SI);
                  end;
               end if;
               Scan_Tree_For_Captures (D (AS_BODY, Node), SK, Owner_Map, Parent_Map, Captured_Vars, Target_Owner);
               return;
            end if;
         end;
      end if;

      if ARITY (Node) = ARBITRARY then
         declare
            L : SEQ_TYPE := LIST (Node);
            Item : TREE;
         begin
            while not IS_EMPTY (L) loop
               POP (L, Item);
               Scan_Tree_For_Captures (Item, Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Target_Owner);
            end loop;
         end;
      else
         for I in 1 .. ARITIES'Pos (ARITY (Node)) loop
            Scan_Tree_For_Captures (DABS (I, Node), Current_Subp, Owner_Map, Parent_Map, Captured_Vars, Target_Owner);
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
      elsif K = DN_VARIABLE_DECL then
         declare
            NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Node));
            ID : TREE;
         begin
            if not IS_EMPTY (NS) then
               POP (NS, ID);
               declare
                  TI : constant Type_Info := Get_Type_Info (Ctx, D (SM_OBJ_TYPE, ID));
               begin
                  if TI.Kind = TK_Unconstrained_Array then
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

   -- =========================================================================
   -- Generic Instantiation Emission
   -- =========================================================================

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
   begin
      -- Skip predefined generic instantiations handled directly by RTL
      if Gen_Name = "INTEGER_IO" or else Gen_Name = "FLOAT_IO"
        or else Gen_Name = "FIXED_IO" or else Gen_Name = "ENUMERATION_IO"
        or else Gen_Name = "UNCHECKED_DEALLOCATION" or else Gen_Name = "UNCHECKED_CONVERSION"
      then
         return;
      end if;

      if not Ctx.Generic_Decls.Contains (Gen_Key) or else not Ctx.Generic_Body.Contains (Gen_Key) then
         return;
      end if;

      declare
         GDecl     : constant TREE := Ctx.Generic_Decls.Element (Gen_Key);
         GBody     : constant TREE := Ctx.Generic_Body.Element (Gen_Key);
         Saved_Sub : constant Tree_Maps.Map := Ctx.Generic_Subst;
         FItems    : SEQ_TYPE := LIST (D (AS_ITEM_S, GDecl));
         ADecls    : SEQ_TYPE := LIST (D (SM_DECL_S, Unit_K));
         FItem, ADecl : TREE;
      begin
         -- Map formal generic parameters -> actual bindings from instantiation's sm_decl_s
         while not IS_EMPTY (FItems) and then not IS_EMPTY (ADecls) loop
            POP (FItems, FItem);
            POP (ADecls, ADecl);
            if KIND (FItem) = DN_TYPE_DECL and then KIND (ADecl) = DN_SUBTYPE_DECL then
               declare
                  F_Tid : constant TREE := D (AS_SOURCE_NAME, FItem);
                  F_Ts  : constant TREE := D (SM_TYPE_SPEC, F_Tid);
                  A_Sid : constant TREE := D (AS_SOURCE_NAME, ADecl);
                  A_Ts  : constant TREE := D (SM_TYPE_SPEC, A_Sid);
               begin
                  Ctx.Generic_Subst.Include (Tree_Key (F_Tid), A_Sid);
                  Ctx.Generic_Subst.Include (Tree_Key (F_Ts), A_Ts);
               end;
            elsif KIND (FItem) = DN_SUBPROG_ENTRY_DECL and then KIND (ADecl) = DN_SUBPROG_ENTRY_DECL then
               declare
                  F_Sid : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, FItem));
                  A_Unit : constant TREE := D (AS_UNIT_KIND, ADecl);
               begin
                  if not Is_Void_Tree (A_Unit) and then KIND (A_Unit) = DN_RENAMES_UNIT then
                     declare
                        A_Sub : constant TREE := Canonical_Def (D (SM_DEFN, D (AS_NAME, A_Unit)));
                     begin
                        Ctx.Generic_Subst.Include (Tree_Key (F_Sid), A_Sub);
                     end;
                  end if;
               end;
            elsif KIND (FItem) = DN_IN and then KIND (ADecl) = DN_CONSTANT_DECL then
               declare
                  F_Id : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, FItem)));
                  A_Id : constant TREE := HEAD (LIST (D (AS_SOURCE_NAME_S, ADecl)));
               begin
                  Ctx.Generic_Subst.Include (Tree_Key (F_Id), A_Id);
               end;
            end if;
         end loop;

         -- For generic subprograms, also map formal parameter IDs -> instantiated parameter IDs
         if KIND (GBody) = DN_SUBPROGRAM_BODY then
            declare
               Inst_Spec : constant TREE := D (SM_SPEC, Inst_Id);
               Gen_Spec  : constant TREE := D (AS_HEADER, GDecl);
               Body_Spec : constant TREE := D (AS_HEADER, GBody);

               procedure Map_Param_Lists (From_Spec, To_Spec : TREE) is
                  FL : SEQ_TYPE := LIST (D (AS_PARAM_S, From_Spec));
                  TL : SEQ_TYPE := LIST (D (AS_PARAM_S, To_Spec));
                  FP, TP : TREE;
               begin
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
               Emit_Subprogram_Body (Ctx, GBody,
                 Override_Id   => Inst_Id,
                 Override_Spec => Inst_Spec,
                 Is_Exported   => Export_All);
            end;
         end if;

         Ctx.Generic_Subst := Saved_Sub;
      end;
   end Emit_Generic_Instantiation;

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
            Can_Id : constant TREE := Canonical_Def (VID);
            Key    : constant Unsigned_32 := Tree_Key (Can_Id);
            Ada_T  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, VID));
            TI     : constant Type_Info := Get_Type_Info (Ctx, Ada_T);
            VInfo  : Var_Info;
         begin
            -- Fast path for compile-time static scalar constants
            if Is_Const and then TI.Kind = TK_Scalar and then not Is_Void_Tree (Init_Exp)
              and then Has_Static_Val (D (SM_VALUE, Init_Exp))
            then
               VInfo.Ada_Type := Ada_T;
               VInfo.Pir_Type := TI.Pir_Type;
               VInfo.Align    := TI.Align;
               VInfo.Owner_Subp := 0;
               if Is_Float_Type (Ada_T) then
                  VInfo.Kind      := VK_Const_Float;
                  VInfo.Const_F64 := Eval_Float64 (D (SM_VALUE, Init_Exp));
               elsif Is_Fixed_Type (Ada_T) then
                  VInfo.Kind      := VK_Const_Int;
                  VInfo.Const_I64 := Integer_64 (IEEE_Float_64'Rounding (Eval_Float64 (D (SM_VALUE, Init_Exp)) / Fixed_Small (Ada_T)));
               else
                  VInfo.Kind      := VK_Const_Int;
                  VInfo.Const_I64 := Eval_Int64 (D (SM_VALUE, Init_Exp));
               end if;
               Ctx.Vars.Include (Key, VInfo);

            elsif Is_Global then
               declare
                  GSym : constant String :=
                    (if Export_All then "@ada_g__" & Qualified_Ada_Name (Can_Id)
                     else "@ada_g__" & Qualified_Ada_Name (Can_Id) & "_" & Tree_Key_Str (Can_Id));
                  Link : constant String := (if Export_All then "export " else "");
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
                              V : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                           begin
                              Emit_Range_Check (Ctx, To_String (V.Reg), To_String (V.Pir_Type), Ada_T);
                              Emit_Inst (Ctx.E,
                                "store." & To_String (TI.Pir_Type) & " " & To_String (V.Reg) &
                                ", " & GA & ", align " & Img (TI.Align));
                           end;
                        else
                           Emit_Init_Composite (Ctx, GA, Ada_T, Init_Exp);
                        end if;
                     end;
                  end if;
               end;

            elsif TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic then
               -- Dynamic-bound local array -> PIR alloca_dyn!
               Has_Dyn := True;
               declare
                  ETI    : constant Type_Info := Get_Type_Info (Ctx, TI.Elem_Type);
                  ESz    : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                  Sub_S  : constant TREE := D (SM_INDEX_SUBTYPE_S, Resolve_Type_Spec (Ada_T));
                  IL     : SEQ_TYPE := LIST (Sub_S);
                  Idx_T  : TREE;
                  Dim    : Natural := 0;
                  Tot64  : Unbounded_String;
               begin
                  VInfo.Kind       := VK_Dyn_Array;
                  VInfo.Ada_Type   := Ada_T;
                  VInfo.Pir_Type   := To_Unbounded_String ("ptr");
                  VInfo.Align      := ETI.Align;
                  VInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                  while not IS_EMPTY (IL) and then Dim < 4 loop
                     POP (IL, Idx_T);
                     Dim := Dim + 1;
                     declare
                        RT, RL, RH : Unbounded_String;
                        F32, L32   : Unbounded_String;
                        Df : constant String := Next_Reg (Ctx.E, "dd");
                        Ln : constant String := Next_Reg (Ctx.E, "dl");
                        Ps : constant String := Next_Reg (Ctx.E, "dp");
                        Cl : constant String := Next_Reg (Ctx.E, "dcl");
                        C6 : constant String := Next_Reg (Ctx.E, "dc6");
                     begin
                        Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (Idx_T)), RT, RL, RH);
                        F32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                        L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                        VInfo.First_Regs (Dim) := F32;
                        VInfo.Last_Regs (Dim)  := L32;
                        Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L32) & ", " & To_String (F32));
                        Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                        Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                        Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                        Emit_Inst (Ctx.E, C6 & " = sext.i64 " & Cl);
                        if Dim = 1 then
                           Tot64 := To_Unbounded_String (C6);
                        else
                           declare
                              M : constant String := Next_Reg (Ctx.E, "dm");
                           begin
                              Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & C6);
                              Tot64 := To_Unbounded_String (M);
                           end;
                        end if;
                     end;
                  end loop;
                  VInfo.NDims := Dim;
                  declare
                     Byt : constant String := Next_Reg (Ctx.E, "dby");
                     Nz  : constant String := Next_Reg (Ctx.E, "dnz");
                     AlB : constant String := Next_Reg (Ctx.E, "dal");
                     Ptr : constant String := Next_Reg (Ctx.E, "darr");
                  begin
                     Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                     Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                     Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                     Emit_Inst (Ctx.E, Ptr & " = alloca_dyn " & AlB & ", align " & Img (ETI.Align));
                     VInfo.Pir_Name := To_Unbounded_String (Ptr);
                     Ctx.Vars.Include (Key, VInfo);
                     if not Is_Void_Tree (Init_Exp) then
                        Emit_Init_Composite (Ctx, Ptr, Ada_T, Init_Exp, VInfo.First_Regs, VInfo.Last_Regs);
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

                  if not Is_Const then
                     -- Mutable unconstrained variable: copy into a fresh Secondary Stack buffer
                     -- so mutating elements never writes to .rodata or aliases another array variable!
                     declare
                        ETI   : constant Type_Info := Get_Type_Info (Ctx, TI.Elem_Type);
                        ESz   : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                        Tot64 : Unbounded_String;
                        Byt   : constant String := Next_Reg (Ctx.E, "uby");
                        Buf_U : Unbounded_String;
                     begin
                        for D_Idx in 1 .. ND loop
                           declare
                              Df : constant String := Next_Reg (Ctx.E, "ud");
                              Ln : constant String := Next_Reg (Ctx.E, "ul");
                              Ps : constant String := Next_Reg (Ctx.E, "up");
                              Cl : constant String := Next_Reg (Ctx.E, "ucl");
                              C6 : constant String := Next_Reg (Ctx.E, "uc6");
                           begin
                              Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (VInfo.Last_Regs (D_Idx)) & ", " & To_String (VInfo.First_Regs (D_Idx)));
                              Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                              Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                              Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                              Emit_Inst (Ctx.E, C6 & " = sext.i64 " & Cl);
                              if D_Idx = 1 then
                                 Tot64 := To_Unbounded_String (C6);
                              else
                                 declare
                                    M : constant String := Next_Reg (Ctx.E, "um");
                                 begin
                                    Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & C6);
                                    Tot64 := To_Unbounded_String (M);
                                 end;
                              end if;
                           end;
                        end loop;
                        Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Byt & ", " & Img (ETI.Align), "ptr", Buf_U);
                        Emit_Inst (Ctx.E, "memcpy " & To_String (Buf_U) & ", " & To_String (Src.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                        Data_Ptr := Buf_U;
                        VInfo.Align := ETI.Align;
                     end;
                  end if;

                  VInfo.Pir_Name := Data_Ptr;
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
                  if Ctx.Vars.Contains (Key) then
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
                           V : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Ada_T);
                           VR : Unbounded_String := V.Reg;
                        begin
                           if To_String (V.Pir_Type) /= To_String (TI.Pir_Type) and then not Is_Float_Type (Ada_T) then
                              VR := To_Unbounded_String (
                                Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type)));
                           end if;
                           Emit_Range_Check (Ctx, To_String (VR), To_String (TI.Pir_Type), Ada_T);
                           Emit_Inst (Ctx.E,
                             "store." & To_String (TI.Pir_Type) & " " & To_String (VR) &
                             ", " & Reg & ", align " & Img (TI.Align));
                        end;
                     else
                        Emit_Init_Composite (Ctx, Reg, Ada_T, Init_Exp);
                     end if;
                  elsif Is_Access_Type (Ada_T) then
                     -- Ada guarantees default null initialization for access objects!
                     Emit_Inst (Ctx.E, "store.ptr 0, " & Reg & ", align 8");
                  elsif Is_Record_Type (Ada_T) then
                     -- Initialize constrained record discriminants (or default discriminants)
                     declare
                        RT : constant TREE := Resolve_Type_Spec (Ada_T);
                     begin
                        if KIND (RT) = DN_CONSTRAINED_RECORD then
                           declare
                              ND : constant TREE := D (SM_NORMALIZED_DSCRMT_S, RT);
                           begin
                              if not Is_Void_Tree (ND) then
                                 declare
                                    DL   : SEQ_TYPE := LIST (ND);
                                    DExp : TREE;
                                    FIdx : Natural := 0;
                                 begin
                                    while not IS_EMPTY (DL) loop
                                       POP (DL, DExp);
                                       declare
                                          DV : constant Expr_Val := Emit_Expr (Ctx, DExp);
                                          FP : constant String := Next_Reg (Ctx.E, "dfp");
                                       begin
                                          Emit_Inst (Ctx.E,
                                            FP & " = field_ptr " & To_String (TI.Pir_Type) & ", " &
                                            Reg & ", " & Img (FIdx));
                                          Emit_Inst (Ctx.E,
                                            "store." & To_String (DV.Pir_Type) & " " & To_String (DV.Reg) &
                                            ", " & FP & ", align " & Img (DV.Align));
                                       end;
                                       FIdx := FIdx + 1;
                                    end loop;
                                 end;
                              end if;
                           end;
                        elsif KIND (RT) = DN_RECORD then
                           declare
                              DS : constant TREE := D (SM_DISCRIMINANT_S, RT);
                           begin
                              if not Is_Void_Tree (DS) then
                                 declare
                                    DL   : SEQ_TYPE := LIST (DS);
                                    DD   : TREE;
                                    FIdx : Natural := 0;
                                 begin
                                    while not IS_EMPTY (DL) loop
                                       POP (DL, DD);
                                       if KIND (DD) = DN_DSCRMT_DECL then
                                          declare
                                             NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                             DID : TREE;
                                             IE  : constant TREE := D (AS_EXP, DD);
                                          begin
                                             while not IS_EMPTY (NS) loop
                                                POP (NS, DID);
                                                if not Is_Void_Tree (IE) then
                                                   declare
                                                      DV : constant Expr_Val := Emit_Expr (Ctx, IE, D (SM_OBJ_TYPE, DID));
                                                      FP : constant String := Next_Reg (Ctx.E, "dfp");
                                                   begin
                                                      Emit_Inst (Ctx.E,
                                                        FP & " = field_ptr " & To_String (TI.Pir_Type) & ", " &
                                                        Reg & ", " & Img (FIdx));
                                                      Emit_Inst (Ctx.E,
                                                        "store." & To_String (DV.Pir_Type) & " " & To_String (DV.Reg) &
                                                        ", " & FP & ", align " & Img (DV.Align));
                                                   end;
                                                end if;
                                                FIdx := FIdx + 1;
                                             end loop;
                                          end;
                                       end if;
                                    end loop;
                                 end;
                              end if;
                           end;
                        end if;
                     end;
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
      Parent_Subp : Unsigned_32)
   is
      Can : constant TREE := Canonical_Def (Subp_Id);
      Key : constant Unsigned_32 := Tree_Key (Can);
      SI  : Subp_Info;
   begin
      if Ctx.Subps.Contains (Key) then
         SI := Ctx.Subps.Element (Key);
      end if;
      if Length (SI.Pir_Name) = 0 then
         SI.Pir_Name := To_Unbounded_String (Mangle_Subp_Name (Ctx, Can, Spec_Node));
      end if;
      SI.Spec_Node   := Spec_Node;
      SI.Parent_Subp := Parent_Subp;
      SI.Is_Function := KIND (Spec_Node) = DN_FUNCTION_SPEC;
      if SI.Is_Function then
         SI.Ret_Type := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, Spec_Node))));
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
      Has_Dyn    : out Boolean)
   is
      L    : SEQ_TYPE;
      Item : TREE;
      Local_Insts : Pending_Inst_Vecs.Vector;
   begin
      Has_Dyn := False;
      if Is_Void_Tree (Item_S) then
         return;
      end if;

      -- Pass 1: register types, exceptions, generic specs/bodies, subprogram specs
      L := LIST (Item_S);
      while not IS_EMPTY (L) loop
         POP (L, Item);
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
                        Dummy : constant String := Get_Exc_Tag (Ctx, EID);
                     begin
                        pragma Unreferenced (Dummy);
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
                  if not Is_Void_Tree (UK) and then KIND (UK) = DN_INSTANTIATION then
                     Pre_Register_Subp (Ctx, SID, D (SM_SPEC, SID), Ctx.Cur_Subp_Key);
                     Local_Insts.Append ((Inst_Decl => Item, Owner_Subp => Ctx.Cur_Subp_Key));
                  else
                     Pre_Register_Subp (Ctx, SID, D (AS_HEADER, Item), Ctx.Cur_Subp_Key);
                  end if;
               end;

            when DN_SUBPROGRAM_BODY =>
               declare
                  SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
               begin
                  if KIND (SID) = DN_GENERIC_ID then
                     Ctx.Generic_Body.Include (Tree_Key (SID), Item);
                  else
                     Pre_Register_Subp (Ctx, SID, D (AS_HEADER, Item), Ctx.Cur_Subp_Key);
                  end if;
               end;

            when DN_PACKAGE_DECL =>
               declare
                  PID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                  UK  : constant TREE := D (AS_UNIT_KIND, Item);
               begin
                  if Is_Void_Tree (UK) then
                     -- Nested package spec: pre-register its subprogram specs
                     declare
                        Hdr : constant TREE := D (AS_HEADER, Item);
                     begin
                        if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                           declare
                              Sub_Dyn : Boolean;
                           begin
                              Emit_Decl_Items (Ctx, D (AS_DECL_S1, Hdr), Is_Global => True, Export_All => Export_All, Has_Dyn => Sub_Dyn);
                              Emit_Decl_Items (Ctx, D (AS_DECL_S2, Hdr), Is_Global => True, Export_All => False, Has_Dyn => Sub_Dyn);
                           end;
                        end if;
                     end;
                  elsif KIND (UK) = DN_INSTANTIATION then
                     -- Instantiated package (e.g. Text_IO.Integer_IO, or user generic package)
                     declare
                        ISpec : constant TREE := D (SM_SPEC, PID);
                     begin
                        if not Is_Void_Tree (ISpec) and then KIND (ISpec) = DN_PACKAGE_SPEC then
                           declare
                              Sub_Dyn : Boolean;
                           begin
                              Emit_Decl_Items (Ctx, D (AS_DECL_S1, ISpec), Is_Global => True, Export_All => Export_All, Has_Dyn => Sub_Dyn);
                           end;
                        end if;
                     end;
                  end if;
               end;

            when others =>
               null;
         end case;
      end loop;

      -- Pass 2: emit variables, constants, renamings, nested package bodies, subprogram bodies, generic instances
      L := LIST (Item_S);
      while not IS_EMPTY (L) loop
         POP (L, Item);
         case KIND (Item) is
            when DN_VARIABLE_DECL | DN_CONSTANT_DECL =>
               Emit_Object_Decl (Ctx, Item, Is_Global, Export_All, Has_Dyn);

            when DN_RENAMES_OBJ_DECL =>
               declare
                  RID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
                  LV  : constant Expr_Val := Emit_LValue (Ctx, D (AS_NAME, Item));
                  V   : Var_Info;
               begin
                  V.Ada_Type   := LV.Ada_Type;
                  V.Pir_Type   := LV.Pir_Type;
                  V.Align      := LV.Align;
                  V.Owner_Subp := Ctx.Cur_Subp_Key;
                  if LV.Kind = EK_Slice then
                     V.Kind       := VK_Dyn_Array;
                     V.Pir_Name   := LV.Reg;
                     V.NDims      := LV.NDims;
                     V.First_Regs := LV.First_Regs;
                     V.Last_Regs  := LV.Last_Regs;
                  else
                     V.Kind     := VK_Param_Ptr;
                     V.Pir_Name := LV.Reg;
                  end if;
                  Ctx.Vars.Include (Tree_Key (RID), V);
               end;

            when DN_PACKAGE_BODY =>
               declare
                  Bdy : constant TREE := D (AS_BODY, Item);
               begin
                  if not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_BLOCK_BODY then
                     declare
                        Sub_Dyn : Boolean;
                     begin
                        Emit_Decl_Items (Ctx, D (AS_ITEM_S, Bdy), Is_Global => True, Export_All => Export_All, Has_Dyn => Sub_Dyn);
                        Emit_Stm_List (Ctx, D (AS_STM_S, Bdy));
                     end;
                  end if;
               end;

            when DN_SUBPROGRAM_BODY =>
               declare
                  SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, Item));
               begin
                  if KIND (SID) /= DN_GENERIC_ID then
                     Emit_Subprogram_Body (Ctx, Item, Is_Exported => Export_All);
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
         exit when Is_Terminated (Ctx.E);
         Emit_Stm (Ctx, S);
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

      -- Reclaim any Secondary Stack temporaries allocated in the failed try body!
      declare
         HE : constant Handler_Entry := Ctx.Handlers.Element (Top_Idx);
         Dummy : Unbounded_String;
      begin
         if Length (HE.SS_Mark) > 0 then
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_release", To_String (HE.SS_Mark), "", Dummy, Nounwind => True);
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

   procedure Emit_Block_Body
     (Ctx        : in out Context;
      Block_Node : TREE;
      Is_Subp    : Boolean)
   is
      Item_S    : constant TREE := D (AS_ITEM_S, Block_Node);
      Stm_S     : constant TREE := D (AS_STM_S, Block_Node);
      Alt_S     : constant TREE := D (AS_ALTERNATIVE_S, Block_Node);
      Has_Hdl   : constant Boolean := (not Is_Void_Tree (Alt_S)) and then (not IS_EMPTY (LIST (Alt_S)));
      Hdl_Lbl   : Unbounded_String;
      After_Lbl : Unbounded_String;
      Saved_SP  : Unbounded_String;
      Has_Dyn   : Boolean := False;
      Has_SS    : Boolean := False;
      Saved_SS  : Unbounded_String;
   begin
      if not Is_Subp then
         declare
            SP : constant String := Next_Reg (Ctx.E, "sp");
         begin
            Emit_Inst (Ctx.E, SP & " = stacksave");
            Saved_SP := To_Unbounded_String (SP);
         end;
         if Tree_Uses_SS (Ctx, Block_Node)
           and then not (Ctx.Cur_Ret_Uncon and then Tree_Has_Return (Block_Node))
         then
            Has_SS := True;
            Emit_Call_Or_Invoke (Ctx, "@__ada_ss_mark", "", "i64", Saved_SS, Nounwind => True);
            Ctx.SS_Marks.Append (Saved_SS);
         end if;
      end if;

      Emit_Decl_Items (Ctx, Item_S, Is_Global => False, Export_All => False, Has_Dyn => Has_Dyn);

      if Has_Hdl then
         Hdl_Lbl   := To_Unbounded_String (Next_Label (Ctx.E, "exc_h"));
         After_Lbl := To_Unbounded_String (Next_Label (Ctx.E, "exc_end"));
         declare
            H_Mark : Unbounded_String := Null_Unbounded_String;
         begin
            if not Ctx.SS_Marks.Is_Empty then
               H_Mark := Ctx.SS_Marks.Last_Element;
            end if;
            Ctx.Handlers.Append ((Handler_Label => Hdl_Lbl,
                                  Current_Exn   => Null_Unbounded_String,
                                  In_Handler    => False,
                                  SS_Mark       => H_Mark));
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

      if not Is_Subp and then Has_Dyn and then not Is_Terminated (Ctx.E) then
         Emit_Inst (Ctx.E, "stackrestore " & To_String (Saved_SP));
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
                        V : constant Integer_64 := Eval_Int64 (D (SM_VALUE, D (AS_EXP, Ch)));
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
                           if Has_Static_Val (D (SM_VALUE, D (AS_EXP1, DR))) and then
                              Has_Static_Val (D (SM_VALUE, D (AS_EXP2, DR)))
                           then
                              Lo := Eval_Int64 (D (SM_VALUE, D (AS_EXP1, DR)));
                              Hi := Eval_Int64 (D (SM_VALUE, D (AS_EXP2, DR)));
                              St := True;
                           end if;
                        elsif KIND (DR) = DN_DISCRETE_SUBTYPE then
                           declare
                              SI : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                           begin
                              if KIND (SI) = DN_SUBTYPE_INDICATION and then not Is_Void_Tree (D (AS_CONSTRAINT, SI)) then
                                 declare
                                    R : constant TREE := D (AS_CONSTRAINT, SI);
                                 begin
                                    Lo := Eval_Int64 (D (SM_VALUE, D (AS_EXP1, R)));
                                    Hi := Eval_Int64 (D (SM_VALUE, D (AS_EXP2, R)));
                                    St := True;
                                 end;
                              else
                                 Get_Discrete_Bounds (D (SM_TYPE_SPEC, Get_Name_Defn (SI)), St, Lo, Hi);
                              end if;
                           end;
                        end if;
                        if St then
                           if Hi >= Lo and then (Hi - Lo) <= 64 then
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
      for WR of Wide_Ranges loop
         declare
            C1   : constant String := Next_Reg (Ctx.E, "wc1");
            C2   : constant String := Next_Reg (Ctx.E, "wc2");
            InR  : constant String := Next_Reg (Ctx.E, "win");
            Next : constant String := Next_Label (Ctx.E, "wnx");
         begin
            Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & PT & " " & To_String (Sel.Reg) & ", " & Img (Long_Long_Integer (WR.Lo)));
            Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & PT & " " & To_String (Sel.Reg) & ", " & Img (Long_Long_Integer (WR.Hi)));
            Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
            Emit_Term (Ctx.E, "br " & InR & ", " & To_String (WR.Lbl) & ", " & Next);
            Emit_Label (Ctx.E, Next);
         end;
      end loop;

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
      Loop_SS   : constant Boolean := Tree_Uses_SS (Ctx, Body_Stms);

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
      Ctx.Loops.Append ((Label_Key  => L_Key,
                         Exit_Label => To_Unbounded_String (L_End),
                         SS_Depth   => Natural (Ctx.SS_Marks.Length)));

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
               C : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Iter));
            begin
               Emit_Term (Ctx.E, "br " & To_String (C.Reg) & ", " & L_Body & ", " & L_End);
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
                  V1 : constant TREE := D (SM_VALUE, D (AS_EXP1, Rng_Node));
                  V2 : constant TREE := D (SM_VALUE, D (AS_EXP2, Rng_Node));
               begin
                  if Has_Static_Val (V1) and then Has_Static_Val (V2) then
                     VInfo.Has_Static_Rng := True;
                     VInfo.Rng_Lo := Eval_Int64 (V1);
                     VInfo.Rng_Hi := Eval_Int64 (V2);
                  end if;
               end;
            end if;
            Ctx.Vars.Include (Tree_Key (Iter_Id), VInfo);

            Start_R := (if Is_Rev then Hi_R else Lo_R);
            Stop_R  := (if Is_Rev then Lo_R else Hi_R);

            -- Check if upper/lower bound is statically safe from wrap (enables canonical 2-block loop for loop_vec/rotate!)
            declare
               Can_Fast : Boolean := False;
               Limit_V  : Integer_64 := 0;
            begin
               if KIND (Rng_Node) = DN_RANGE then
                  declare
                     Bound_N : constant TREE := (if Is_Rev then D (AS_EXP1, Rng_Node) else D (AS_EXP2, Rng_Node));
                     BV      : constant TREE := D (SM_VALUE, Bound_N);
                     Bits    : constant Natural := Scalar_Bits (Ada_T);
                  begin
                     if Has_Static_Val (BV) then
                        Limit_V := Eval_Int64 (BV);
                        if not Is_Rev then
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
                     L_Hdr : constant String := Next_Label (Ctx.E, "for_h");
                     Cmp_C : constant String := Next_Reg (Ctx.E, "fcmp");
                     Cmp_Op : constant String := (if Is_Rev then "cmp_ge_s." else "cmp_le_s.");
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
                           Emit_Inst (Ctx.E, Next_V & " = sub." & To_String (PT) & " " & Cur_V & ", 1 [unsafe_s]");
                        else
                           Emit_Inst (Ctx.E, Next_V & " = add." & To_String (PT) & " " & Cur_V & ", 1 [unsafe_s]");
                        end if;
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Next_V & ")");
                     end if;
                  end;
               else
                  -- Wrap-safe loop when bound is dynamic or equals Type'Last / Type'First!
                  Emit_Inst (Ctx.E, Empty_C & " = cmp_gt_s." & To_String (PT) & " " & To_String (Lo_R) & ", " & To_String (Hi_R));
                  Emit_Term (Ctx.E, "br " & Empty_C & ", " & L_End & ", " & L_Body & "(" & To_String (Start_R) & ")");

                  Emit_Label (Ctx.E, L_Body, Cur_V & ": " & To_String (PT));
                  Emit_Inst (Ctx.E, "store." & To_String (PT) & " " & Cur_V & ", " & Var_Reg & ", align " & Img (TI.Align));
                  Emit_Body_With_SS;
                  if not Is_Terminated (Ctx.E) then
                     Emit_Inst (Ctx.E, Done_C & " = cmp_eq." & To_String (PT) & " " & Cur_V & ", " & To_String (Stop_R));
                     Emit_Term (Ctx.E, "br " & Done_C & ", " & L_End & ", " & L_Step);
                     Emit_Label (Ctx.E, L_Step);
                     if Is_Rev then
                        Emit_Inst (Ctx.E, Next_V & " = sub." & To_String (PT) & " " & Cur_V & ", 1 [unsafe_s]");
                     else
                        Emit_Inst (Ctx.E, Next_V & " = add." & To_String (PT) & " " & Cur_V & ", 1 [unsafe_s]");
                     end if;
                     Emit_Term (Ctx.E, "jmp " & L_Body & "(" & Next_V & ")");
                  end if;
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
                     Lbl : constant String := ".ada_lbl_" & Tree_Key_Str (LID);
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
               LDef : constant TREE := D (SM_DEFN, D (AS_NAME, Stm));
            begin
               Emit_Term (Ctx.E, "jmp .ada_lbl_" & Tree_Key_Str (LDef));
            end;

         when DN_ASSIGN =>
            declare
               LHS : constant Expr_Val := Emit_LValue (Ctx, D (AS_NAME, Stm));
               DTI : constant Type_Info := Get_Type_Info (Ctx, LHS.Ada_Type);
               RHS_Node : constant TREE := D (AS_EXP, Stm);
            begin
               if DTI.Kind = TK_Scalar then
                  declare
                     RHS : constant Expr_Val := Emit_Expr (Ctx, RHS_Node, LHS.Ada_Type);
                     VR  : Unbounded_String := RHS.Reg;
                  begin
                     if To_String (RHS.Pir_Type) /= To_String (DTI.Pir_Type) and then not Is_Float_Type (LHS.Ada_Type) then
                        VR := To_Unbounded_String (
                          Emit_Int_Convert (Ctx, To_String (RHS.Reg), To_String (RHS.Pir_Type), To_String (DTI.Pir_Type)));
                     end if;
                     Emit_Range_Check (Ctx, To_String (VR), To_String (DTI.Pir_Type), LHS.Ada_Type);
                     Emit_Inst (Ctx.E,
                       "store." & To_String (DTI.Pir_Type) & " " & To_String (VR) &
                       ", " & To_String (LHS.Reg) & ", align " & Img (DTI.Align));
                  end;
               elsif KIND (RHS_Node) = DN_AGGREGATE then
                  Emit_Init_Composite (Ctx, To_String (LHS.Reg), LHS.Ada_Type, RHS_Node, LHS.First_Regs, LHS.Last_Regs);
               else
                  declare
                     RHS : constant Expr_Val := Emit_Expr (Ctx, RHS_Node, LHS.Ada_Type);
                  begin
                     -- Length check for slice / array assignment if enabled
                     if Ctx.Enable_Checks and then (LHS.Kind = EK_Slice or else RHS.Kind = EK_Slice) then
                        declare
                           LF, LL, RF, RL : Unbounded_String;
                           D1 : constant String := Next_Reg (Ctx.E, "ld1");
                           N1 : constant String := Next_Reg (Ctx.E, "ln1");
                           P1 : constant String := Next_Reg (Ctx.E, "lp1");
                           C1 : constant String := Next_Reg (Ctx.E, "lc1");
                           D2 : constant String := Next_Reg (Ctx.E, "ld2");
                           N2 : constant String := Next_Reg (Ctx.E, "ln2");
                           P2 : constant String := Next_Reg (Ctx.E, "lp2");
                           C2 : constant String := Next_Reg (Ctx.E, "lc2");
                           Bad : constant String := Next_Reg (Ctx.E, "lbad");
                           Fail_L : constant String := Next_Label (Ctx.E, "lcf");
                           Ok_L   : constant String := Next_Label (Ctx.E, "lco");
                        begin
                           Get_Array_Dim_Bounds (Ctx, LHS, 1, LF, LL);
                           Get_Array_Dim_Bounds (Ctx, RHS, 1, RF, RL);
                           Emit_Inst (Ctx.E, D1 & " = sub.i32 " & To_String (LL) & ", " & To_String (LF));
                           Emit_Inst (Ctx.E, N1 & " = add.i32 " & D1 & ", 1");
                           Emit_Inst (Ctx.E, P1 & " = cmp_gt_s.i32 " & N1 & ", 0");
                           Emit_Inst (Ctx.E, C1 & " = select.i32 " & P1 & ", " & N1 & ", 0");
                           Emit_Inst (Ctx.E, D2 & " = sub.i32 " & To_String (RL) & ", " & To_String (RF));
                           Emit_Inst (Ctx.E, N2 & " = add.i32 " & D2 & ", 1");
                           Emit_Inst (Ctx.E, P2 & " = cmp_gt_s.i32 " & N2 & ", 0");
                           Emit_Inst (Ctx.E, C2 & " = select.i32 " & P2 & ", " & N2 & ", 0");
                           Emit_Inst (Ctx.E, Bad & " = cmp_ne.i32 " & C1 & ", " & C2);
                           Emit_Term (Ctx.E, "br " & Bad & ", " & Fail_L & ", " & Ok_L);
                           Emit_Label (Ctx.E, Fail_L);
                           Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                           Emit_Label (Ctx.E, Ok_L);
                        end;
                     end if;
                     Emit_Copy_Composite (Ctx, To_String (LHS.Reg), RHS, LHS.Ada_Type);
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
                        L_T   : constant String := Next_Label (Ctx.E, "if_t");
                        L_F   : constant String := Next_Label (Ctx.E, "if_f");
                     begin
                        Emit_Term (Ctx.E, "br " & To_String (Cond.Reg) & ", " & L_T & ", " & L_F);
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
               Target_Loop : constant TREE := D (SM_STM, Stm);
               Cond_Exp    : constant TREE := D (AS_EXP, Stm);
               Exit_Lbl    : Unbounded_String;
               Exit_SS_Dep : Natural := Natural (Ctx.SS_Marks.Length);
            begin
               if not Is_Void_Tree (Target_Loop) then
                  declare
                     LID : constant TREE := D (AS_SOURCE_NAME, Target_Loop);
                     LK  : constant Unsigned_32 := (if Is_Void_Tree (LID) then 0 else Tree_Key (LID));
                  begin
                     for I in reverse 0 .. Natural (Ctx.Loops.Length) - 1 loop
                        if LK /= 0 and then Ctx.Loops.Element (I).Label_Key = LK then
                           Exit_Lbl    := Ctx.Loops.Element (I).Exit_Label;
                           Exit_SS_Dep := Ctx.Loops.Element (I).SS_Depth;
                           exit;
                        end if;
                     end loop;
                  end;
               end if;
               if Length (Exit_Lbl) = 0 and then not Ctx.Loops.Is_Empty then
                  Exit_Lbl    := Ctx.Loops.Last_Element.Exit_Label;
                  Exit_SS_Dep := Ctx.Loops.Last_Element.SS_Depth;
               end if;

               if Is_Void_Tree (Cond_Exp) then
                  Emit_SS_Release_To (Ctx, Exit_SS_Dep);
                  Emit_Term (Ctx.E, "jmp " & To_String (Exit_Lbl));
               else
                  declare
                     C     : constant Expr_Val := Emit_Expr (Ctx, Cond_Exp);
                     L_Ex  : constant String := Next_Label (Ctx.E, "ex_t");
                     L_Cnt : constant String := Next_Label (Ctx.E, "ex_c");
                  begin
                     if Natural (Ctx.SS_Marks.Length) > Exit_SS_Dep then
                        Emit_Term (Ctx.E, "br " & To_String (C.Reg) & ", " & L_Ex & ", " & L_Cnt);
                        Emit_Label (Ctx.E, L_Ex);
                        Emit_SS_Release_To (Ctx, Exit_SS_Dep);
                        Emit_Term (Ctx.E, "jmp " & To_String (Exit_Lbl));
                        Emit_Label (Ctx.E, L_Cnt);
                     else
                        Emit_Term (Ctx.E, "br " & To_String (C.Reg) & ", " & To_String (Exit_Lbl) & ", " & L_Cnt);
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
               begin
                  for CO of Ctx.Copy_Outs loop
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
                  end loop;
               end Emit_Copy_Outs;
            begin
               if Is_Void_Tree (Exp) then
                  Emit_Copy_Outs;
                  Emit_SS_Release_To (Ctx, 0);
                  Emit_Term (Ctx.E, "ret");
               elsif Ctx.Cur_Ret_Uncon then
                  -- Unconstrained or dynamic-bound array return -> allocate result on Secondary Stack
                  -- and populate caller's descriptor { ptr data, i32 f1, i32 l1, ... } at %__ret!
                  declare
                     RTI     : constant Type_Info := Get_Type_Info (Ctx, Ctx.Cur_Ret_Type);
                     ETI     : constant Type_Info := Get_Type_Info (Ctx, RTI.Elem_Type);
                     ESz     : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                     ND      : constant Natural := (if RTI.NDims > 0 then RTI.NDims else 1);
                     Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, ND);
                     Firsts  : Str_Array_4;
                     Lasts   : Str_Array_4;
                     Tot64   : Unbounded_String;
                     SS_Ptr  : Unbounded_String;
                     Unw_Exp : TREE := Exp;
                  begin
                     while not Is_Void_Tree (Unw_Exp)
                       and then (KIND (Unw_Exp) = DN_QUALIFIED or else KIND (Unw_Exp) = DN_PARENTHESIZED)
                     loop
                        Unw_Exp := D (AS_EXP, Unw_Exp);
                     end loop;

                     if KIND (Unw_Exp) = DN_AGGREGATE then
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
                           -- Unconstrained array aggregate: infer bounds from range choice or positional count
                           declare
                              Assocs : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, Unw_Exp));
                              Item   : TREE;
                              Count  : Integer_64 := 0;
                              Got_R  : Boolean := False;
                           begin
                              while not IS_EMPTY (Assocs) loop
                                 POP (Assocs, Item);
                                 if KIND (Item) = DN_ASSOC or else KIND (Item) = DN_NAMED then
                                    declare
                                       CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, Item));
                                       Ch : TREE;
                                    begin
                                       while not IS_EMPTY (CS) loop
                                          POP (CS, Ch);
                                          if KIND (Ch) = DN_CHOICE_RANGE then
                                             declare
                                                RT, RL, RH : Unbounded_String;
                                             begin
                                                Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                                Firsts (1) := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                                                Lasts (1)  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                                                Got_R := True;
                                             end;
                                          end if;
                                       end loop;
                                    end;
                                 else
                                    Count := Count + 1;
                                 end if;
                              end loop;
                              if not Got_R then
                                 declare
                                    F1    : constant String := Next_Reg (Ctx.E, "raf");
                                    L1    : constant String := Next_Reg (Ctx.E, "ral");
                                    Idx_St : Boolean := False;
                                    Idx_Lo, Idx_Hi : Integer_64 := 1;
                                    Base_Lo : Integer_64 := 1;
                                 begin
                                    if not Is_Void_Tree (RTI.Dims (1).Idx_Type) then
                                       Get_Discrete_Bounds (RTI.Dims (1).Idx_Type, Idx_St, Idx_Lo, Idx_Hi);
                                       if Idx_St then
                                          Base_Lo := Idx_Lo;
                                       end if;
                                    end if;
                                    Emit_Inst (Ctx.E, F1 & " = const.i32 " & Img (Long_Long_Integer (Base_Lo)));
                                    Emit_Inst (Ctx.E, L1 & " = const.i32 " & Img (Long_Long_Integer (Base_Lo + Count - 1)));
                                    Firsts (1) := To_Unbounded_String (F1);
                                    Lasts (1)  := To_Unbounded_String (L1);
                                 end;
                              end if;
                           end;
                        end if;

                        for D_Idx in 1 .. ND loop
                           declare
                              Df : constant String := Next_Reg (Ctx.E, "rd");
                              Ln : constant String := Next_Reg (Ctx.E, "rl");
                              Ps : constant String := Next_Reg (Ctx.E, "rp");
                              Cl : constant String := Next_Reg (Ctx.E, "rcl");
                              C6 : constant String := Next_Reg (Ctx.E, "rc6");
                           begin
                              Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (Lasts (D_Idx)) & ", " & To_String (Firsts (D_Idx)));
                              Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                              Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                              Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                              Emit_Inst (Ctx.E, C6 & " = sext.i64 " & Cl);
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
                        declare
                           Byt : constant String := Next_Reg (Ctx.E, "rby");
                        begin
                           Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Byt & ", " & Img (ETI.Align), "ptr", SS_Ptr);
                        end;
                        Emit_Init_Composite (Ctx, To_String (SS_Ptr), Ctx.Cur_Ret_Type, Unw_Exp, Firsts, Lasts);
                     else
                        -- General array expression: evaluate, allocate on Secondary Stack, copy elements!
                        declare
                           Src : constant Expr_Val := Emit_Expr (Ctx, Exp, Ctx.Cur_Ret_Type);
                           Byt : constant String := Next_Reg (Ctx.E, "rby");
                        begin
                           for D_Idx in 1 .. ND loop
                              Get_Array_Dim_Bounds (Ctx, Src, D_Idx, Firsts (D_Idx), Lasts (D_Idx));
                              declare
                                 Df : constant String := Next_Reg (Ctx.E, "rd");
                                 Ln : constant String := Next_Reg (Ctx.E, "rl");
                                 Ps : constant String := Next_Reg (Ctx.E, "rp");
                                 Cl : constant String := Next_Reg (Ctx.E, "rcl");
                                 C6 : constant String := Next_Reg (Ctx.E, "rc6");
                              begin
                                 Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (Lasts (D_Idx)) & ", " & To_String (Firsts (D_Idx)));
                                 Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                                 Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                                 Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                                 Emit_Inst (Ctx.E, C6 & " = sext.i64 " & Cl);
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
                  Emit_Init_Composite (Ctx, To_String (Ctx.Cur_Ret_Ptr), Ctx.Cur_Ret_Type, Exp);
                  Emit_Copy_Outs;
                  Emit_SS_Release_To (Ctx, 0);
                  Emit_Term (Ctx.E, "ret");
               else
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, Ctx.Cur_Ret_Type);
                     V   : constant Expr_Val := Emit_Expr (Ctx, Exp, Ctx.Cur_Ret_Type);
                     VR  : Unbounded_String := V.Reg;
                  begin
                     if To_String (V.Pir_Type) /= To_String (RTI.Pir_Type) and then not Is_Float_Type (Ctx.Cur_Ret_Type) then
                        VR := To_Unbounded_String (
                          Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (RTI.Pir_Type)));
                     end if;
                     Emit_Range_Check (Ctx, To_String (VR), To_String (RTI.Pir_Type), Ctx.Cur_Ret_Type);
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

         when DN_PROCEDURE_CALL =>
            declare
               Dummy : constant Expr_Val := Emit_Subp_Call (Ctx, Stm, Is_Func => False);
            begin
               pragma Unreferenced (Dummy);
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
      Is_Exported   : Boolean := False)
   is
      Subp_Id   : constant TREE :=
        (if not Is_Void_Tree (Override_Id) then Canonical_Def (Override_Id)
         else Canonical_Def (D (AS_SOURCE_NAME, Body_Node)));
      Spec_Node : constant TREE :=
        (if not Is_Void_Tree (Override_Spec) then Override_Spec
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
      Saved_Is_Func   : constant Boolean := Ctx.Cur_Is_Func;
      Saved_Loops     : constant Loop_Vecs.Vector := Ctx.Loops;
      Saved_Handlers  : constant Handler_Vecs.Vector := Ctx.Handlers;
      Saved_Copy_Outs : constant Copy_Out_Vecs.Vector := Ctx.Copy_Outs;
      Saved_SS_Marks  : constant Str_Vecs.Vector := Ctx.SS_Marks;
      Saved_Uplevel   : constant Type_Maps.Map := Ctx.Uplevel_Cache;

      Captured_Vars  : U32_Vecs.Vector;
   begin
      Pre_Register_Subp (Ctx, Subp_Id, Spec_Node, Saved_Key);
      SI := Ctx.Subps.Element (Subp_Key);

      -- Pre-register local variables & parameters in Owner_Map for static-link capture scan
      declare
         Temp_Owners : Var_Maps.Map;
         PS : constant TREE := D (AS_PARAM_S, Spec_Node);
         IS_N : constant TREE := D (AS_ITEM_S, Block_B);
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
         if not Is_Void_Tree (IS_N) then
            declare
               IL : SEQ_TYPE := LIST (IS_N);
               It : TREE;
            begin
               while not IS_EMPTY (IL) loop
                  POP (IL, It);
                  if KIND (It) = DN_VARIABLE_DECL or else KIND (It) = DN_CONSTANT_DECL then
                     declare
                        NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, It));
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
                  end if;
               end loop;
            end;
         end if;
         Scan_Tree_For_Captures (Block_B, Subp_Key, Temp_Owners, Ctx.Subps, Captured_Vars, Subp_Key);
      end;

      -- Reload SI (Needs_SL might have been set by outer scan!)
      SI := Ctx.Subps.Element (Subp_Key);
      if not Captured_Vars.Is_Empty then
         SI.Has_Frame := True;
         SI.Frame_Type := To_Unbounded_String ("@__frame_" & Tree_Key_Str (Subp_Id));
         -- Emit frame struct: { ptr parent_sl, ptr cap1, ptr cap2, ... }
         declare
            FT_Decl : Unbounded_String := To_Unbounded_String ("type " & To_String (SI.Frame_Type) & " = { ptr");
         begin
            for Slot in 1 .. Natural (Captured_Vars.Length) loop
               Append (FT_Decl, ", ptr");
               declare
                  CK : constant Unsigned_32 := Captured_Vars.Element (Slot - 1);
                  V  : Var_Info;
               begin
                  if Ctx.Vars.Contains (CK) then
                     V := Ctx.Vars.Element (CK);
                  end if;
                  V.Captured_Slot := Slot;
                  V.Owner_Subp := Subp_Key;
                  Ctx.Vars.Include (CK, V);
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
      Ctx.Cur_SL_Reg    := To_Unbounded_String (if SI.Needs_SL then "%__sl" else "");
      Ctx.Cur_Frame_Reg := Null_Unbounded_String;
      Ctx.Loops.Clear;
      Ctx.Handlers.Clear;
      Ctx.Copy_Outs.Clear;
      Ctx.SS_Marks.Clear;
      Ctx.Uplevel_Cache.Clear;

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
                        if Ctx.Vars.Contains (PK) then
                           VInfo.Captured_Slot := Ctx.Vars.Element (PK).Captured_Slot;
                        end if;
                        VInfo.Ada_Type   := PT;
                        VInfo.Pir_Type   := TI.Pir_Type;
                        VInfo.Align      := TI.Align;
                        VInfo.Owner_Subp := Subp_Key;

                        if TI.Kind = TK_Unconstrained_Array then
                           VInfo.Kind     := VK_Uncon_Array_Param;
                           VInfo.Pir_Name := To_Unbounded_String (PN & "_ptr");
                           VInfo.NDims    := TI.NDims;
                           for D_Idx in 1 .. TI.NDims loop
                              VInfo.First_Regs (D_Idx) := To_Unbounded_String (PN & "_f" & Img (D_Idx));
                              VInfo.Last_Regs (D_Idx)  := To_Unbounded_String (PN & "_l" & Img (D_Idx));
                           end loop;
                        elsif TI.Kind = TK_Record or else TI.Kind = TK_Constrained_Array then
                           VInfo.Kind     := VK_Param_Ptr;
                           VInfo.Pir_Name := To_Unbounded_String (PN);
                           if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
                              declare
                                 FP : constant String := Next_Reg (Ctx.E, "pfp");
                              begin
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (SI.Frame_Type) & ", " &
                                   To_String (Ctx.Cur_Frame_Reg) & ", " & Img (VInfo.Captured_Slot));
                                 Emit_Inst (Ctx.E, "store.ptr " & PN & ", " & FP & ", align 8");
                              end;
                           end if;
                        elsif Mode = DN_OUT or else Mode = DN_IN_OUT then
                           -- Scalar OUT / IN OUT: Ada 83 LRM 6.2(7) copy-in / copy-out semantics!
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
                        else
                           -- Scalar IN parameter
                           if VInfo.Captured_Slot > 0 and then SI.Has_Frame then
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
      Ctx.Cur_Frame_Reg := Saved_Frame;
      Ctx.Cur_SL_Reg    := Saved_SL;
      Ctx.Cur_Is_Func   := Saved_Is_Func;
      Ctx.Loops         := Saved_Loops;
      Ctx.Handlers      := Saved_Handlers;
      Ctx.Copy_Outs     := Saved_Copy_Outs;
      Ctx.SS_Marks      := Saved_SS_Marks;
      Ctx.Uplevel_Cache := Saved_Uplevel;
   end Emit_Subprogram_Body;

   -- =========================================================================
   -- Top-Level Compilation Driver
   -- =========================================================================

   procedure Emit_Main_Wrapper (Ctx : in out Context; Entry_Sym : String; With_Units : SEQ_TYPE) is
      Saved : Pir_Emit.Func_Buffer;
      WList : SEQ_TYPE := With_Units;
      TW    : TREE;
   begin
      Begin_Func (Ctx.E, Saved, "export func @main() -> i32 cc ""c""");

      -- Call elaborators of any non-RTL withed user packages
      while not IS_EMPTY (WList) loop
         POP (WList, TW);
         declare
            CU : constant TREE := D (TW_COMP_UNIT, TW);
            AD : constant TREE := D (AS_ALL_DECL, CU);
         begin
            if KIND (AD) = DN_PACKAGE_DECL then
               declare
                  PN : constant String := Sym_Name (D (AS_SOURCE_NAME, AD));
               begin
                  if PN /= "_STANDRD" and then PN /= "STANDARD" and then PN /= "SYSTEM"
                    and then PN /= "TEXT_IO" and then PN /= "IO_EXCEPTIONS"
                    and then PN /= "CALENDAR"
                  then
                     declare
                        ESym : constant String := "@ada__" & Lower_Name (PN) & "__elab_spec";
                        BSym : constant String := "@ada__" & Lower_Name (PN) & "__elab_body";
                     begin
                        Add_Extern_Func (Ctx.E, ESym, "extern func " & ESym & "() -> void cc ""internal""");
                        Emit_Inst (Ctx.E, "call " & ESym & "()");
                        if DB (XD_BODY_IS_REQUIRED, D (AS_HEADER, AD)) then
                           Add_Extern_Func (Ctx.E, BSym, "extern func " & BSym & "() -> void cc ""internal""");
                           Emit_Inst (Ctx.E, "call " & BSym & "()");
                        end if;
                     end;
                  end if;
               end;
            end if;
         end;
      end loop;

      -- Invoke Ada main procedure with top-level unhandled-exception handler
      Emit_Term (Ctx.E, "invoke " & Entry_Sym & "() then .done unwind .unhandled(exn)");
      Emit_Label (Ctx.E, ".done");
      Emit_Term (Ctx.E, "ret 0");
      Emit_Label (Ctx.E, ".unhandled", "%exn: ptr");
      Emit_Inst (Ctx.E, "call @__ada_unhandled_exception(%exn)");
      Emit_Term (Ctx.E, "unreachable");

      End_Func (Ctx.E, Saved);
   end Emit_Main_Wrapper;

   procedure Emit_Compilation (Ctx : in out Context; Comp_Node : TREE) is
      CU_List : SEQ_TYPE := LIST (D (AS_COMPLTN_UNIT_S, Comp_Node));
      CU      : TREE;
   begin
      Ensure_Runtime_Externs (Ctx);

      while not IS_EMPTY (CU_List) loop
         POP (CU_List, CU);
         declare
            All_Decl : constant TREE := D (AS_ALL_DECL, CU);
            K        : constant NODE_NAME := KIND (All_Decl);
         begin
            if K = DN_SUBPROGRAM_BODY then
               Emit_Subprogram_Body (Ctx, All_Decl, Is_Exported => True);
               if Ctx.Emit_Main then
                  declare
                     SID : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                     SI  : constant Subp_Info := Ctx.Subps.Element (Tree_Key (SID));
                  begin
                     Emit_Main_Wrapper (Ctx, To_String (SI.Pir_Name), LIST (CU));
                  end;
               end if;

            elsif K = DN_PACKAGE_DECL then
               declare
                  PID   : constant TREE := D (AS_SOURCE_NAME, All_Decl);
                  PName : constant String := Lower_Name (Sym_Name (PID));
                  Hdr   : constant TREE := D (AS_HEADER, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
                  Dyn1, Dyn2 : Boolean;
               begin
                  Begin_Func (Ctx.E, Saved,
                    "export func @ada__" & PName & "__elab_spec() -> void cc ""internal""");
                  if not Is_Void_Tree (Hdr) and then KIND (Hdr) = DN_PACKAGE_SPEC then
                     Emit_Decl_Items (Ctx, D (AS_DECL_S1, Hdr), Is_Global => True, Export_All => True, Has_Dyn => Dyn1);
                     Emit_Decl_Items (Ctx, D (AS_DECL_S2, Hdr), Is_Global => True, Export_All => True, Has_Dyn => Dyn2);
                  end if;
                  if not Is_Terminated (Ctx.E) then
                     Emit_Term (Ctx.E, "ret");
                  end if;
                  End_Func (Ctx.E, Saved);
               end;

            elsif K = DN_PACKAGE_BODY then
               declare
                  PID   : constant TREE := Canonical_Def (D (AS_SOURCE_NAME, All_Decl));
                  PName : constant String := Lower_Name (Sym_Name (PID));
                  Bdy   : constant TREE := D (AS_BODY, All_Decl);
                  Saved : Pir_Emit.Func_Buffer;
                  Dyn   : Boolean;
               begin
                  Begin_Func (Ctx.E, Saved,
                    "export func @ada__" & PName & "__elab_body() -> void cc ""internal""");
                  if not Is_Void_Tree (Bdy) and then KIND (Bdy) = DN_BLOCK_BODY then
                     Emit_Decl_Items (Ctx, D (AS_ITEM_S, Bdy), Is_Global => True, Export_All => True, Has_Dyn => Dyn);
                     Emit_Stm_List (Ctx, D (AS_STM_S, Bdy));
                  end if;
                  if not Is_Terminated (Ctx.E) then
                     Emit_Term (Ctx.E, "ret");
                  end if;
                  End_Func (Ctx.E, Saved);
               end;
            end if;
         end;
      end loop;
   end Emit_Compilation;

end Cg_Stmt;
