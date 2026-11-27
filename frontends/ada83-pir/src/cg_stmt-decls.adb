-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Fixed;
with Ada.Strings.Maps.Constants;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr; use Cg_Expr;
with Lib_Binder;

package body Cg_Stmt.Decls is

   -- Shared helper: check if AN is a named array aggregate without `others` across Rem_D dimensions
   function Is_Named_No_Others_Agg (AN : TREE; Rem_D : Natural) return Boolean is
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
            if Rem_D > 1 and then not Is_Named_No_Others_Agg (D (AS_EXP, IT), Rem_D - 1) then
               return False;
            end if;
         end loop;
         return Has_Any;
      end;
   end Is_Named_No_Others_Agg;

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
                           pragma Warnings (Off, PT_U); pragma Warnings (Off, Lo_U); pragma Warnings (Off, Hi_U);
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
                           if TI.Kind = TK_Constrained_Array
                             and then (KIND (Init_Exp) /= DN_AGGREGATE
                                       or else Is_Named_No_Others_Agg (Init_Exp, Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims))))
                           then
                              declare
                                 ND_A   : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims));
                                 Eval_T : constant TREE := (if KIND (Init_Exp) = DN_AGGREGATE then Subst_Base_Type (Ctx, Ada_T) else Ada_T);
                                 SV     : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Eval_T);
                                 Tgt    : Expr_Val;
                              begin
                                 Tgt.Kind := EK_Slice; Tgt.Reg := PtrR; Tgt.Ada_Type := Ada_T; Tgt.NDims := ND_A;
                                 for D_I in 1 .. ND_A loop
                                    Emit_Index_Bounds (Ctx, TI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                                 end loop;
                                 Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                                 Emit_Copy_Composite (Ctx, To_String (PtrR), SV, Ada_T);
                              end;
                           else
                              Emit_Init_Composite (Ctx, To_String (PtrR), Ada_T, Init_Exp);
                              if TI.Kind = TK_Record then
                                 Emit_Discriminant_Check (Ctx, To_String (PtrR), Ada_T, Is_Access => False);
                              end if;
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
                                 F32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64", Is_Unsigned => Uns_I));
                                 L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64", Is_Unsigned => Uns_I));
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
                           if KIND (Init_Exp) = DN_AGGREGATE and then Is_Named_No_Others_Agg (Init_Exp, ND) then
                              declare
                                 SV  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Subst_Base_Type (Ctx, Ada_T));
                                 Tgt : Expr_Val;
                              begin
                                 Tgt.Kind := EK_Slice; Tgt.Reg := Data_Ptr; Tgt.Ada_Type := Ada_T;
                                 Tgt.NDims := ND; Tgt.First_Regs := VInfo.First_Regs; Tgt.Last_Regs := VInfo.Last_Regs;
                                 Emit_Array_Length_Check (Ctx, Tgt, SV, ND);
                                 Emit_Copy_Composite (Ctx, To_String (Data_Ptr), SV, Ada_T);
                              end;
                           else
                              Emit_Init_Composite (Ctx, To_String (Data_Ptr), Ada_T, Init_Exp, VInfo.First_Regs, VInfo.Last_Regs);
                           end if;
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
                              DP : constant String := To_String (TI.Pir_Type);
                              VR : constant String :=
                                Emit_Typed_Scalar_Convert
                                  (Ctx, To_String (V.Reg), V.Ada_Type, To_String (V.Pir_Type),
                                   Ada_T, DP, Check_Range => True);
                           begin
                              Emit_Discriminant_Check (Ctx, VR, Ada_T, Is_Access => True);
                              Emit_Inst (Ctx.E,
                                "store." & DP & " " & VR &
                                ", " & GA & ", align " & Img (TI.Align));
                           end;
                        elsif TI.Kind = TK_Constrained_Array
                          and then (KIND (Init_Exp) /= DN_AGGREGATE
                                    or else Is_Named_No_Others_Agg (Init_Exp, Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims))))
                        then
                           declare
                              Eval_T : constant TREE := (if KIND (Init_Exp) = DN_AGGREGATE then Subst_Base_Type (Ctx, Ada_T) else Ada_T);
                              SV  : constant Expr_Val := Emit_Expr (Ctx, Init_Exp, Eval_T);
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

            elsif TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic
              and then not (Is_Const and then Is_Void_Tree (Init_Exp))
            then
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
                           F32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64", Is_Unsigned => Uns_I));
                           L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64", Is_Unsigned => Uns_I));
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
                        if KIND (Init_Exp) = DN_AGGREGATE and then Is_Named_No_Others_Agg (Init_Exp, Dim) then
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
                           DP : constant String := To_String (TI.Pir_Type);
                           VR : constant String :=
                             Emit_Typed_Scalar_Convert
                               (Ctx, To_String (V.Reg), V.Ada_Type, To_String (V.Pir_Type),
                                Ada_T, DP, Check_Range => True);
                        begin
                           Emit_Discriminant_Check (Ctx, VR, Ada_T, Is_Access => True);
                           Emit_Inst (Ctx.E,
                             "store." & DP & " " & VR &
                             ", " & Reg & ", align " & Img (TI.Align));
                        end;
                     elsif TI.Kind = TK_Constrained_Array then
                        declare
                           ND_A : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims));
                        begin
                           if KIND (Init_Exp) /= DN_AGGREGATE or else Is_Named_No_Others_Agg (Init_Exp, ND_A) then
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
                           begin
                              Add_Data_Decl (Ctx.E, Sym,
                                "export " & Sym & " = data align 1 { """ & Escape_Pir_String (Sym_Name (Can)) & "\0"" }");
                              Ctx.Exc_Tags.Include (Key, To_Unbounded_String (Sym));
                           end;
                        elsif In_Gen_B then
                           declare
                              Sym : constant String := "@__ada_exc_" & QName & "_" & Tree_Key_Str (Can) & To_String (Ctx.Gen_Inst_Tag);
                           begin
                              Add_Data_Decl (Ctx.E, Sym,
                                Sym & " = data align 1 { """ & Escape_Pir_String (Sym_Name (Can)) & "\0"" }");
                              Ctx.Gen_Local_Exc_Tags.Include (Key, To_Unbounded_String (Sym));
                           end;
                        else
                           -- P3-5.10: Define local exception data directly so subunit-local exceptions don't hit Get_Exc_Tag extern fallback!
                           declare
                              Sym : constant String := "@__ada_exc_" & QName & "_" & Tree_Key_Str (Can);
                           begin
                              Add_Data_Decl (Ctx.E, Sym,
                                Sym & " = data align 1 { """ & Escape_Pir_String (Sym_Name (Can)) & "\0"" }");
                              Ctx.Exc_Tags.Include (Key, To_Unbounded_String (Sym));
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
                                    pragma Warnings (Off, PT_U); pragma Warnings (Off, Lo_U); pragma Warnings (Off, Hi_U);
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
                                       Ctx.Access_Masters.Include (Tree_Key (BS),
                                         (Local_Ptr  => To_Unbounded_String (MA),
                                          Global_Sym => To_Unbounded_String (MSym),
                                          Owner_Subp => Ctx.Cur_Subp_Key,
                                          Cap_Slot   => Cap_Sl));
                                    end;
                                 else
                                    declare
                                       MSym : constant String := "@ada_accmst_" & Tree_Key_Str (BS) & To_String (Ctx.Gen_Inst_Tag);
                                       MA   : constant String := Next_Reg (Ctx.E, "amst");
                                    begin
                                       Add_Global_Decl (Ctx.E, MSym, MSym & " = global ptr = zeroinit, align 8");
                                       Emit_Inst (Ctx.E, MA & " = addr " & MSym);
                                       Emit_Inst (Ctx.E, "store.ptr " & To_String (Ctx.Cur_Master_Reg) & ", " & MA & ", align 8");
                                       Ctx.Access_Masters.Include (Tree_Key (BS),
                                         (Local_Ptr  => Null_Unbounded_String,
                                          Global_Sym => To_Unbounded_String (MSym),
                                          Owner_Subp => 0,
                                          Cap_Slot   => 0));
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
                           begin
                              Add_Global_Decl (Ctx.E, GSym, Link & GSym & " = global @__ada_arr_desc = zeroinit, align 8");
                              Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                              Emit_Store_Arr_Desc (Ctx, GA, To_String (LV.Reg), LV.First_Regs, LV.Last_Regs, LV.NDims);
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
                        begin
                           Emit_Entry_Alloca (Ctx.E, Desc_R & " = alloca @__ada_arr_desc, align 8");
                           Emit_Store_Arr_Desc (Ctx, Desc_R, To_String (LV.Reg), LV.First_Regs, LV.Last_Regs, LV.NDims);
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
                           Emit_Entry_Alloca (Ctx.E, Sp & " = alloca " & To_String (LV.Pir_Type) & ", align " & Img (Natural'Max (1, TI.Align)));
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
                              -- Also run deferred Init_Exp now that the address clause pointer is bound (LRM 13.5, P3-6.10, P4-5)
                              -- Never clobber an overlaid non-task object when there is no explicit initializer!
                              if not Is_Void_Tree (Init_E) then
                                 if TI_D.Kind = TK_Scalar then
                                    declare
                                       IV : constant Expr_Val := Emit_Expr (Ctx, Init_E, V.Ada_Type);
                                       DP : constant String := To_String (TI_D.Pir_Type);
                                       VR : constant String :=
                                         Emit_Typed_Scalar_Convert
                                           (Ctx, To_String (IV.Reg), IV.Ada_Type, To_String (IV.Pir_Type),
                                            V.Ada_Type, DP, Check_Range => True);
                                    begin
                                       Emit_Discriminant_Check (Ctx, VR, V.Ada_Type, Is_Access => True);
                                       Emit_Inst (Ctx.E, "store." & DP & " " & VR & ", " & PtrR & ", align " & Img (TI_D.Align));
                                    end;
                                 elsif TI_D.Kind = TK_Constrained_Array
                                   and then (KIND (Init_E) /= DN_AGGREGATE
                                             or else Is_Named_No_Others_Agg (Init_E, Natural'Max (1, Natural'Min (TI_D.NDims, Max_Array_Dims))))
                                 then
                                    declare
                                       ND_A   : constant Natural := Natural'Max (1, Natural'Min (TI_D.NDims, Max_Array_Dims));
                                       Eval_T : constant TREE := (if KIND (Init_E) = DN_AGGREGATE then Subst_Base_Type (Ctx, V.Ada_Type) else V.Ada_Type);
                                       SV     : constant Expr_Val := Emit_Expr (Ctx, Init_E, Eval_T);
                                       Tgt    : Expr_Val;
                                    begin
                                       Tgt.Kind := EK_Slice; Tgt.Reg := To_Unbounded_String (PtrR); Tgt.Ada_Type := V.Ada_Type; Tgt.NDims := ND_A;
                                       for D_I in 1 .. ND_A loop
                                          Emit_Index_Bounds (Ctx, TI_D.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                                       end loop;
                                       Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                                       Emit_Copy_Composite (Ctx, PtrR, SV, V.Ada_Type);
                                    end;
                                 else
                                    Emit_Init_Composite (Ctx, PtrR, V.Ada_Type, Init_E);
                                    if TI_D.Kind = TK_Record then
                                       Emit_Discriminant_Check (Ctx, PtrR, V.Ada_Type, Is_Access => False);
                                    end if;
                                 end if;
                              elsif Is_Task_Type (Subst_Resolve_Type (Ctx, V.Ada_Type)) then
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
                  begin
                     Add_Global_Decl (Ctx.E, GSym, Link_G & GSym & " = global i8 = 0, align 1");
                     Emit_Inst (Ctx.E, GA & " = addr " & GSym);
                     Emit_Inst (Ctx.E, "store.i8 0, " & GA & ", align 1");
                     Ctx.Gen_Elab_Flags.Include (GK, To_Unbounded_String (GSym));
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
                                       if Is_Void_Tree (D (AS_UNIT_KIND, I2))
                                         and then (Is_Void_Tree (D (AS_SOURCE_NAME, I2))
                                                   or else KIND (D (AS_SOURCE_NAME, I2)) not in DN_PROCEDURE_ID | DN_FUNCTION_ID | DN_OPERATOR_ID
                                                   or else Is_Void_Tree (D (SM_INTERFACE, D (AS_SOURCE_NAME, I2))))
                                       then
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
                           GSym : constant String := To_String (Ctx.Gen_Elab_Flags.Element (Tree_Key (PID)));
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
                                                 SS_Mark       => Latest_Cleanup_Reg (Ctx, CK_SS_Mark),
                                                 SP_Mark       => Latest_Cleanup_Reg (Ctx, CK_SP_Mark)));
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
                        Scan_Label_Cleanup_Depths (Ctx, D (AS_STM_S, Bdy), Cleanup_Depth (Ctx));
                        Scan_Label_Cleanup_Depths (Ctx, Alt_S, Cleanup_Depth (Ctx));
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
                                             ST_Def : constant TREE :=
                                               (if KIND (SI_N) = DN_SUBTYPE_INDICATION then Get_Name_Defn (D (AS_NAME, SI_N))
                                                else Get_Name_Defn (SI_N));
                                             ST_T   : constant TREE :=
                                               (if not Is_Void_Tree (ST_Def) and then KIND (ST_Def) in CLASS_TYPE_NAME
                                                then Subst_Tree (Ctx, D (SM_TYPE_SPEC, ST_Def)) else CONST_VOID);
                                             PT_U, Lo_R, Hi_R : Unbounded_String;
                                             Uns_B : Boolean;
                                          begin
                                             Emit_Discrete_Range (Ctx, DR, PT_U, Lo_R, Hi_R);
                                             Uns_B := Is_Unsigned_Scalar (ST_T, To_String (PT_U));
                                             Lo32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (PT_U), "i32", Is_Unsigned => Uns_B));
                                             Hi32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (PT_U), "i32", Is_Unsigned => Uns_B));
                                             Have_Dyn_Bounds := True;
                                          end;
                                       end if;
                                    end;
                                 elsif not Is_Void_Tree (DR) and then KIND (DR) in DN_RANGE | DN_RANGE_ATTRIBUTE then
                                    declare
                                       PT_U, Lo_R, Hi_R : Unbounded_String;
                                       Uns_B : Boolean;
                                    begin
                                       if Ctx.Enable_Checks or else (Has_Dyn_E and then Has_EI_E and then not EI_E.Fam_Is_Static) then
                                          Emit_Discrete_Range (Ctx, DR, PT_U, Lo_R, Hi_R);
                                          Uns_B := Is_Unsigned_Scalar (D (SM_TYPE_SPEC, DR), To_String (PT_U));
                                          Lo32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (PT_U), "i32", Is_Unsigned => Uns_B));
                                          Hi32_E := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (PT_U), "i32", Is_Unsigned => Uns_B));
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
                           GSym : constant String := To_String (Ctx.Gen_Elab_Flags.Element (Tree_Key (SID)));
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
   end Emit_Decl_Items;

end Cg_Stmt.Decls;
