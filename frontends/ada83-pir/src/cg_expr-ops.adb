-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;

package body Cg_Expr.Ops is

   -- Emit a built-in operator call (from DN_BLTN_OPERATOR_ID)
   function Emit_Builtin_Op
     (Ctx        : in out Context;
      Op_Id      : TREE;
      Param_List : SEQ_TYPE;
      Res_Type   : TREE) return Expr_Val
   is
      Op_Pos : constant Integer := DI (SM_OPERATOR, Op_Id);
      -- Matches PRENAME.OP_CLASS order:
      -- 0:and, 1:or, 2:xor, 3:not, 4:u+, 5:u-, 6:abs,
      -- 7:=, 8:/=, 9:<, 10:<=, 11:>, 12:>=,
      -- 13:+, 14:-, 15:*, 16:/, 17:mod, 18:rem, 19:&, 20:**
      PL   : SEQ_TYPE := Param_List;
      A1_N : TREE := CONST_VOID;
      A2_N : TREE := CONST_VOID;
      Res  : Expr_Val;
      RTI  : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
   begin
      if not IS_EMPTY (PL) then
         POP (PL, A1_N);
      end if;
      if not IS_EMPTY (PL) then
         POP (PL, A2_N);
      end if;

      Res.Kind     := EK_Scalar;
      Res.Ada_Type := Res_Type;
      Res.Pir_Type := RTI.Pir_Type;
      Res.Align    := RTI.Align;

      -- Unary ops (3:not, 4:u+, 5:u-, 6:abs)
      if Op_Pos in 3 .. 6 then
         declare
            V  : constant Expr_Val := Emit_Expr (Ctx, A1_N, Res_Type);
            PT : constant String := To_String (V.Pir_Type);
            R  : constant String := Next_Reg (Ctx.E, "u");
         begin
            Res.Pir_Type := V.Pir_Type;
            if Op_Pos = 3 then -- NOT
               if Is_Array_Type (V.Ada_Type) then
                  -- 1D Boolean array NOT (element-wise on Secondary Stack)
                  declare
                     F1, L1 : Unbounded_String;
                     Df     : constant String := Next_Reg (Ctx.E, "nd");
                     Ln     : constant String := Next_Reg (Ctx.E, "nl");
                     Ps     : constant String := Next_Reg (Ctx.E, "np");
                     Cl     : constant String := Next_Reg (Ctx.E, "ncl");
                     C6     : constant String := Next_Reg (Ctx.E, "nc6");
                     Buf_U  : Unbounded_String;
                     L_Hdr  : constant String := Next_Label (Ctx.E, "not_h");
                     L_Bdy  : constant String := Next_Label (Ctx.E, "not_b");
                     L_End  : constant String := Next_Label (Ctx.E, "not_e");
                     IR     : constant String := Next_Reg (Ctx.E, "ni");
                     Cnd    : constant String := Next_Reg (Ctx.E, "nc");
                     SP     : constant String := Next_Reg (Ctx.E, "nsp");
                     DP     : constant String := Next_Reg (Ctx.E, "ndp");
                     BV     : constant String := Next_Reg (Ctx.E, "nbv");
                     NV     : constant String := Next_Reg (Ctx.E, "nnv");
                     Nx     : constant String := Next_Reg (Ctx.E, "nnx");
                  begin
                     Get_Array_Dim_Bounds (Ctx, V, 1, F1, L1);
                     declare
                        BT    : constant TREE := Base_Type_Spec (V.Ada_Type);
                        CompT : constant TREE := (if not Is_Void_Tree (BT) and then KIND (BT) in DN_ARRAY | DN_CONSTRAINED_ARRAY
                                                  then Subst_Tree (Ctx, D (SM_COMP_TYPE, BT)) else CONST_VOID);
                        ETI   : constant Type_Info := Get_Type_Info (Ctx, CompT);
                        EP    : constant String := (if not Is_Void_Tree (CompT) then To_String (ETI.Pir_Type) else "i8");
                        EAl   : constant Natural := (if not Is_Void_Tree (CompT) then Natural'Max (1, ETI.Align) else 1);
                        ESz   : constant Natural := (if not Is_Void_Tree (CompT) then Natural'Max (1, ETI.Byte_Size) else 1);
                        Len64 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (F1), To_String (L1));
                        Byt64 : constant String := Next_Reg (Ctx.E, "nb64");
                        Off64 : constant String := Next_Reg (Ctx.E, "nof");
                     begin
                        Emit_Inst (Ctx.E, Byt64 & " = mul.i64 " & Len64 & ", " & Img (ESz));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Byt64 & ", " & Img (EAl), "ptr", Buf_U);
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(0)");
                        Emit_Label (Ctx.E, L_Hdr, IR & ": i64");
                        Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Len64);
                        Emit_Term (Ctx.E, "br " & Cnd & ", " & L_Bdy & ", " & L_End);
                        Emit_Label (Ctx.E, L_Bdy);
                        Emit_Inst (Ctx.E, Off64 & " = mul.i64 " & IR & ", " & Img (ESz));
                        Emit_Inst (Ctx.E, SP & " = ptr_add.i64 " & To_String (V.Reg) & ", " & Off64 & " [inbounds]");
                        Emit_Inst (Ctx.E, DP & " = ptr_add.i64 " & To_String (Buf_U) & ", " & Off64 & " [inbounds]");
                        Emit_Inst (Ctx.E, BV & " = load." & EP & " " & SP & ", align " & Img (EAl));
                        Emit_Inst (Ctx.E, NV & " = xor." & EP & " " & BV & ", 1");
                        if not Is_Void_Tree (CompT) then
                           Emit_Range_Check (Ctx, NV, EP, CompT);
                        end if;
                        Emit_Inst (Ctx.E, "store." & EP & " " & NV & ", " & DP & ", align " & Img (EAl));
                        Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nx & ")");
                        Emit_Label (Ctx.E, L_End);
                        Res.Kind           := EK_Slice;
                        Res.Reg            := Buf_U;
                        Res.Pir_Type       := To_Unbounded_String ("ptr");
                        Res.Ada_Type       := Res_Type;
                        Res.Align          := EAl;
                        Res.NDims          := 1;
                        Res.First_Regs (1) := F1;
                        Res.Last_Regs (1)  := L1;
                        return Res;
                     end;
                  end;
               end if;
               Emit_Inst (Ctx.E, R & " = xor." & PT & " " & To_String (V.Reg) & ", 1");
               Res.Reg := To_Unbounded_String (R);
            elsif Op_Pos = 4 then -- unary +
               Res.Reg := V.Reg;
            elsif Op_Pos = 5 then -- unary -
               if PT = "f32" or else PT = "f64" then
                  Emit_Inst (Ctx.E, R & " = fneg." & PT & " " & To_String (V.Reg));
                  Res.Reg := To_Unbounded_String (R);
               else
                  -- P3-1.3: perform integer unary minus in T'Base signed width (prevents 'Size-packed i8/i16 overflow!)
                  declare
                     W_PT : Unbounded_String := To_Unbounded_String (PT);
                     VR_W : Unbounded_String := V.Reg;
                     UnsV : constant Boolean := Is_Unsigned_Scalar (V.Ada_Type, PT);
                  begin
                     if Is_Integer_Type (V.Ada_Type) and then (PT = "i8" or else PT = "i16") then
                        declare
                           St_B : Boolean;
                           BLo, BHi : Integer_64;
                        begin
                           Get_Discrete_Bounds (Base_Type_Spec (Subst_Tree (Ctx, V.Ada_Type)), St_B, BLo, BHi);
                           if not St_B or else BLo < -32768 or else BHi > 32767 then
                              W_PT := To_Unbounded_String ("i32");
                           elsif PT = "i8" then
                              W_PT := To_Unbounded_String ("i16");
                           end if;
                           if UnsV and then To_String (W_PT) = PT then
                              W_PT := To_Unbounded_String (if PT = "i8" then "i16" else "i32");
                           end if;
                        end;
                        if To_String (W_PT) /= PT then
                           VR_W := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), PT, To_String (W_PT), Is_Unsigned => UnsV));
                        end if;
                     end if;
                     if Ctx.Enable_Checks then
                        declare
                           Ovf : constant String := Next_Reg (Ctx.E, "ov");
                        begin
                           Emit_Inst (Ctx.E, R & ", " & Ovf & " = sub_ovf_s." & To_String (W_PT) & " 0, " & To_String (VR_W));
                           Emit_Check_Fail_If (Ctx, Ovf);
                        end;
                     else
                        Emit_Inst (Ctx.E, R & " = sub." & To_String (W_PT) & " 0, " & To_String (VR_W) & " [unsafe_s]");
                     end if;
                     Res.Reg      := To_Unbounded_String (R);
                     Res.Pir_Type := W_PT;
                  end;
               end if;
            elsif Op_Pos = 6 then -- ABS
               if PT = "f32" or else PT = "f64" then
                  declare
                     Neg : constant String := Next_Reg (Ctx.E, "fn");
                     Cnd : constant String := Next_Reg (Ctx.E, "fc");
                  begin
                     Emit_Inst (Ctx.E, Neg & " = fneg." & PT & " " & To_String (V.Reg));
                     Emit_Inst (Ctx.E, Cnd & " = fcmp_olt." & PT & " " & To_String (V.Reg) & ", 0.0");
                     Emit_Inst (Ctx.E, R & " = select." & PT & " " & Cnd & ", " & Neg & ", " & To_String (V.Reg));
                     Res.Reg := To_Unbounded_String (R);
                  end;
               else
                  declare
                     W_PT : Unbounded_String := To_Unbounded_String (PT);
                     VR_W : Unbounded_String := V.Reg;
                     UnsV : constant Boolean := Is_Unsigned_Scalar (V.Ada_Type, PT);
                  begin
                     if Is_Integer_Type (V.Ada_Type) and then (PT = "i8" or else PT = "i16") then
                        declare
                           St_B : Boolean;
                           BLo, BHi : Integer_64;
                        begin
                           Get_Discrete_Bounds (Base_Type_Spec (Subst_Tree (Ctx, V.Ada_Type)), St_B, BLo, BHi);
                           if not St_B or else BLo < -32768 or else BHi > 32767 then
                              W_PT := To_Unbounded_String ("i32");
                           elsif PT = "i8" then
                              W_PT := To_Unbounded_String ("i16");
                           end if;
                           if UnsV and then To_String (W_PT) = PT then
                              W_PT := To_Unbounded_String (if PT = "i8" then "i16" else "i32");
                           end if;
                        end;
                        if To_String (W_PT) /= PT then
                           VR_W := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), PT, To_String (W_PT), Is_Unsigned => UnsV));
                        end if;
                     end if;
                     if Ctx.Enable_Checks then
                        declare
                           Is_Min : constant String := Next_Reg (Ctx.E, "amin");
                        begin
                           Emit_Inst (Ctx.E, Is_Min & " = cmp_eq." & To_String (W_PT) & " " & To_String (VR_W) & ", " &
                             Img (Min_Signed_For_Pir (To_String (W_PT))));
                           Emit_Check_Fail_If (Ctx, Is_Min);
                        end;
                     end if;
                     declare
                        Neg : constant String := Next_Reg (Ctx.E, "an");
                        Cnd : constant String := Next_Reg (Ctx.E, "ac");
                     begin
                        Emit_Inst (Ctx.E, Neg & " = sub." & To_String (W_PT) & " 0, " & To_String (VR_W));
                        Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s." & To_String (W_PT) & " " & To_String (VR_W) & ", 0");
                        Emit_Inst (Ctx.E, R & " = select." & To_String (W_PT) & " " & Cnd & ", " & Neg & ", " & To_String (VR_W));
                        Res.Reg      := To_Unbounded_String (R);
                        Res.Pir_Type := W_PT;
                     end;
                  end;
               end if;
            end if;
            return Res;
         end;
      end if;

      -- String/Array concatenation (& = 19)
      if Op_Pos = 19 then
         declare
            BT      : constant TREE := Subst_Base_Type (Ctx, Res_Type);
            BTI     : constant Type_Info := Get_Type_Info (Ctx, BT);
            Comp_T  : constant TREE := Subst_Tree (Ctx, D (SM_COMP_TYPE, BT));
            E1_T    : constant TREE := (if not Is_Void_Tree (A1_N) and then KIND (A1_N) in CLASS_EXP then Subst_Tree (Ctx, D (SM_EXP_TYPE, A1_N)) else CONST_VOID);
            E2_T    : constant TREE := (if not Is_Void_Tree (A2_N) and then KIND (A2_N) in CLASS_EXP then Subst_Tree (Ctx, D (SM_EXP_TYPE, A2_N)) else CONST_VOID);
            L_Is_Elem : constant Boolean :=
              (not Is_Void_Tree (E1_T) and then Tree_Key (Subst_Base_Type (Ctx, E1_T)) /= Tree_Key (BT));
            R_Is_Elem : constant Boolean :=
              (not Is_Void_Tree (E2_T) and then Tree_Key (Subst_Base_Type (Ctx, E2_T)) /= Tree_Key (BT));
            L   : constant Expr_Val := Emit_Expr (Ctx, A1_N, (if L_Is_Elem then Comp_T else BT));
            R_V : constant Expr_Val := Emit_Expr (Ctx, A2_N, (if R_Is_Elem then Comp_T else BT));
            ETI : constant Type_Info := Get_Type_Info (Ctx, Comp_T);
            Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
            EAl : constant Natural := Elem_Storage_Align (ETI);
            ESz : constant Natural := Elem_Storage_Size (ETI);
            L_Len, R_Len : Unbounded_String;
            L_Ptr, R_Ptr : Unbounded_String;
         begin
            if L_Is_Elem then
               declare
                  One : constant String := Next_Reg (Ctx.E, "c1");
               begin
                  Emit_Inst (Ctx.E, One & " = const.i64 1");
                  L_Len := To_Unbounded_String (One);
                  if L.Kind = EK_Scalar then
                     declare
                        Tmp : constant String := Next_Reg (Ctx.E, "ce");
                        LV  : constant String := Emit_Scalar_Coerce (Ctx, To_String (L.Reg), To_String (L.Pir_Type), To_String (ETI.Pir_Type),
                                                   Is_Unsigned => Is_Unsigned_Scalar (L.Ada_Type, To_String (L.Pir_Type)));
                     begin
                        Emit_Range_Check (Ctx, LV, To_String (ETI.Pir_Type), Comp_T);
                        Emit_Discriminant_Check (Ctx, LV, Comp_T, Is_Access => True);
                        Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (ETI.Pir_Type) & ", align " & Img (EAl));
                        Emit_Inst (Ctx.E, "store." & To_String (ETI.Pir_Type) & " " & LV & ", " & Tmp & ", align " & Img (EAl));
                        L_Ptr := To_Unbounded_String (Tmp);
                     end;
                  elsif Is_DE then
                     declare
                        TmpD : constant String := Next_Reg (Ctx.E, "ced");
                        Tgt  : Expr_Val;
                        ND_E : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                     begin
                        Tgt.Kind := EK_Slice; Tgt.Ada_Type := Comp_T; Tgt.NDims := ND_E;
                        for D_I in 1 .. ND_E loop
                           Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                        end loop;
                        Emit_Array_Length_Check (Ctx, Tgt, L, ND_E);
                        Emit_Entry_Alloca (Ctx.E, TmpD & " = alloca @__ada_arr_desc, align 8");
                        Emit_Store_Arr_Desc (Ctx, TmpD, To_String (L.Reg), Tgt.First_Regs, Tgt.Last_Regs, ND_E);
                        L_Ptr := To_Unbounded_String (TmpD);
                     end;
                  else
                     if ETI.Kind = TK_Record then
                        Emit_Discriminant_Check (Ctx, To_String (L.Reg), Comp_T, Is_Access => False);
                     elsif ETI.Kind = TK_Constrained_Array then
                        declare
                           Tgt  : Expr_Val;
                           ND_E : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                        begin
                           Tgt.Kind := EK_Slice; Tgt.Ada_Type := Comp_T; Tgt.NDims := ND_E;
                           for D_I in 1 .. ND_E loop
                              Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                           end loop;
                           Emit_Array_Length_Check (Ctx, Tgt, L, ND_E);
                        end;
                     end if;
                     L_Ptr := L.Reg;
                  end if;
               end;
            else
               declare
                  F, Hi : Unbounded_String;
                  Cl64  : Unbounded_String;
               begin
                  Get_Array_Dim_Bounds (Ctx, L, 1, F, Hi);
                  Cl64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F), To_String (Hi)));
                  L_Ptr := L.Reg;
                  L_Len := Cl64;
               end;
            end if;

            if R_Is_Elem then
               declare
                  One : constant String := Next_Reg (Ctx.E, "c1");
               begin
                  Emit_Inst (Ctx.E, One & " = const.i64 1");
                  R_Len := To_Unbounded_String (One);
                  if R_V.Kind = EK_Scalar then
                     declare
                        Tmp : constant String := Next_Reg (Ctx.E, "ce");
                        RV  : constant String := Emit_Scalar_Coerce (Ctx, To_String (R_V.Reg), To_String (R_V.Pir_Type), To_String (ETI.Pir_Type),
                                                   Is_Unsigned => Is_Unsigned_Scalar (R_V.Ada_Type, To_String (R_V.Pir_Type)));
                     begin
                        Emit_Range_Check (Ctx, RV, To_String (ETI.Pir_Type), Comp_T);
                        Emit_Discriminant_Check (Ctx, RV, Comp_T, Is_Access => True);
                        Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (ETI.Pir_Type) & ", align " & Img (EAl));
                        Emit_Inst (Ctx.E, "store." & To_String (ETI.Pir_Type) & " " & RV & ", " & Tmp & ", align " & Img (EAl));
                        R_Ptr := To_Unbounded_String (Tmp);
                     end;
                  elsif Is_DE then
                     declare
                        TmpD : constant String := Next_Reg (Ctx.E, "ced");
                        Tgt  : Expr_Val;
                        ND_E : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                     begin
                        Tgt.Kind := EK_Slice; Tgt.Ada_Type := Comp_T; Tgt.NDims := ND_E;
                        for D_I in 1 .. ND_E loop
                           Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                        end loop;
                        Emit_Array_Length_Check (Ctx, Tgt, R_V, ND_E);
                        Emit_Entry_Alloca (Ctx.E, TmpD & " = alloca @__ada_arr_desc, align 8");
                        Emit_Store_Arr_Desc (Ctx, TmpD, To_String (R_V.Reg), Tgt.First_Regs, Tgt.Last_Regs, ND_E);
                        R_Ptr := To_Unbounded_String (TmpD);
                     end;
                  else
                     if ETI.Kind = TK_Record then
                        Emit_Discriminant_Check (Ctx, To_String (R_V.Reg), Comp_T, Is_Access => False);
                     elsif ETI.Kind = TK_Constrained_Array then
                        declare
                           Tgt  : Expr_Val;
                           ND_E : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                        begin
                           Tgt.Kind := EK_Slice; Tgt.Ada_Type := Comp_T; Tgt.NDims := ND_E;
                           for D_I in 1 .. ND_E loop
                              Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                           end loop;
                           Emit_Array_Length_Check (Ctx, Tgt, R_V, ND_E);
                        end;
                     end if;
                     R_Ptr := R_V.Reg;
                  end if;
               end;
            else
               declare
                  F, Hi : Unbounded_String;
                  Cl64  : Unbounded_String;
               begin
                  Get_Array_Dim_Bounds (Ctx, R_V, 1, F, Hi);
                  Cl64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F), To_String (Hi)));
                  R_Ptr := R_V.Reg;
                  R_Len := Cl64;
               end;
            end if;

            declare
               Tot64  : constant String := Next_Reg (Ctx.E, "ctot64");
               Bytes  : constant String := Next_Reg (Ctx.E, "cbytes");
               Buf_U  : Unbounded_String;
               LBy    : constant String := Next_Reg (Ctx.E, "clby");
               RBy    : constant String := Next_Reg (Ctx.E, "crby");
               Dst2   : constant String := Next_Reg (Ctx.E, "cdst2");
               Idx_Lo, Idx_Hi : Unbounded_String;
               Res_F  : Unbounded_String;
               Last64 : constant String := Next_Reg (Ctx.E, "cl64");
            begin
               Emit_Inst (Ctx.E, Tot64 & " = add.i64 " & To_String (L_Len) & ", " & To_String (R_Len));
               Emit_Inst (Ctx.E, Bytes & " = mul.i64 " & Tot64 & ", " & Img (ESz));
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Bytes & ", " & Img (EAl), "ptr", Buf_U);
               Emit_Inst (Ctx.E, LBy & " = mul.i64 " & To_String (L_Len) & ", " & Img (ESz));
               Emit_Inst (Ctx.E, "memcpy " & To_String (Buf_U) & ", " & To_String (L_Ptr) & ", " & LBy & ", align " & Img (EAl));
               Emit_Inst (Ctx.E, Dst2 & " = ptr_add.i64 " & To_String (Buf_U) & ", " & LBy & " [inbounds]");
               Emit_Inst (Ctx.E, RBy & " = mul.i64 " & To_String (R_Len) & ", " & Img (ESz));
               Emit_Inst (Ctx.E, "memcpy " & Dst2 & ", " & To_String (R_Ptr) & ", " & RBy & ", align " & Img (EAl));
               -- LRM 4.5.3(5): result lower bound
               Emit_Index_Bounds (Ctx, BTI.Dims (1).Idx_Type, Idx_Lo, Idx_Hi);
               declare
                  Has_L_Non_Null : Boolean := False;
                  L_NN_Reg       : Unbounded_String;
                  Saved_RL_R     : Unbounded_String;
               begin
               if L_Is_Elem then
                  Res_F := Idx_Lo;
               else
                  declare
                     LF_R, LL_R : Unbounded_String;
                     L_Non_Null : constant String := Next_Reg (Ctx.E, "clnn");
                     Alt_F      : Unbounded_String;
                     Pick_F     : constant String := Next_Reg (Ctx.E, "cpf");
                  begin
                     Get_Array_Dim_Bounds (Ctx, L, 1, LF_R, LL_R);
                     if not R_Is_Elem then
                        declare
                           RF_R, RL_R : Unbounded_String;
                        begin
                           Get_Array_Dim_Bounds (Ctx, R_V, 1, RF_R, RL_R);
                           Alt_F := RF_R;
                           Saved_RL_R := RL_R;
                        end;
                     else
                        Alt_F := Idx_Lo;
                     end if;
                     Emit_Inst (Ctx.E, L_Non_Null & " = cmp_gt_s.i64 " & To_String (L_Len) & ", 0");
                     Emit_Inst (Ctx.E, Pick_F & " = select.i64 " & L_Non_Null & ", " & To_String (LF_R) & ", " & To_String (Alt_F));
                     Res_F := To_Unbounded_String (Pick_F);
                     Has_L_Non_Null := True;
                     L_NN_Reg := To_Unbounded_String (L_Non_Null);
                  end;
               end if;
               declare
                  Tm1 : constant String := Next_Reg (Ctx.E, "ctm1");
               begin
                  Emit_Inst (Ctx.E, Tm1 & " = sub.i64 " & Tot64 & ", 1");
                  Emit_Inst (Ctx.E, Last64 & " = add.i64 " & To_String (Res_F) & ", " & Tm1);
               end;
               if Ctx.Enable_Checks then
                  declare
                     NZ  : constant String := Next_Reg (Ctx.E, "cnz");
                     GT  : constant String := Next_Reg (Ctx.E, "cgt");
                     Bad : constant String := Next_Reg (Ctx.E, "cbad");
                  begin
                     Emit_Inst (Ctx.E, NZ  & " = cmp_gt_s.i64 " & Tot64 & ", 0");
                     Emit_Inst (Ctx.E, GT  & " = cmp_gt_s.i64 " & Last64 & ", " & To_String (Idx_Hi));
                     Emit_Inst (Ctx.E, Bad & " = and.i8 " & NZ & ", " & GT);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;
               declare
                  Final_L64 : Unbounded_String := To_Unbounded_String (Last64);
               begin
                  if Has_L_Non_Null and then not R_Is_Elem and then Length (Saved_RL_R) > 0 then
                     declare
                        PL : constant String := Next_Reg (Ctx.E, "cpl");
                     begin
                        Emit_Inst (Ctx.E, PL & " = select.i64 " & To_String (L_NN_Reg) & ", " & Last64 & ", " & To_String (Saved_RL_R));
                        Final_L64 := To_Unbounded_String (PL);
                     end;
                  end if;
                  Res.Kind           := EK_Slice;
                  Res.Reg            := Buf_U;
                  Res.Pir_Type       := To_Unbounded_String ("ptr");
                  Res.Ada_Type       := BT;
                  Res.Align          := ETI.Align;
                  Res.NDims          := 1;
                  Res.First_Regs (1) := Res_F;
                  Res.Last_Regs (1)  := Final_L64;
               end;
               end;
               return Res;
            end;
         end;
      end if;

      -- Binary ops
      declare
         E1_T : constant TREE := (if not Is_Void_Tree (A1_N) and then KIND (A1_N) in CLASS_EXP
                                    and then KIND (A1_N) /= DN_USED_NAME_ID and then KIND (A1_N) /= DN_USED_OP
                                  then Subst_Tree (Ctx, D (SM_EXP_TYPE, A1_N)) else CONST_VOID);
         E2_T : constant TREE := (if not Is_Void_Tree (A2_N) and then KIND (A2_N) in CLASS_EXP
                                    and then KIND (A2_N) /= DN_USED_NAME_ID and then KIND (A2_N) /= DN_USED_OP
                                  then Subst_Tree (Ctx, D (SM_EXP_TYPE, A2_N)) else CONST_VOID);
         T_A1 : TREE := CONST_VOID;
         T_A2 : TREE := CONST_VOID;
      begin
         if Is_Fixed_Type (Res_Type) and then Op_Pos in 13 .. 16 then
            if not Is_Integer_Type (E1_T) then T_A1 := Res_Type; end if;
            if not Is_Integer_Type (E2_T) then T_A2 := Res_Type; end if;
         elsif Op_Pos in 7 .. 12 then
            if Is_Fixed_Type (E2_T) and then not Is_Fixed_Type (E1_T) then T_A1 := E2_T; end if;
            if Is_Fixed_Type (E1_T) and then not Is_Fixed_Type (E2_T) then T_A2 := E1_T; end if;
            if Is_Discrete_Type (E2_T) and then not Is_Void_Tree (E1_T) and then KIND (Resolve_Type_Spec (E1_T)) = DN_UNIVERSAL_INTEGER then
               T_A1 := Subst_Base_Type (Ctx, E2_T);
            end if;
            if Is_Discrete_Type (E1_T) and then not Is_Void_Tree (E2_T) and then KIND (Resolve_Type_Spec (E2_T)) = DN_UNIVERSAL_INTEGER then
               T_A2 := Subst_Base_Type (Ctx, E1_T);
            end if;
         elsif Op_Pos in 13 .. 18 and then Is_Integer_Type (Res_Type) then
            if not Is_Void_Tree (E1_T) and then KIND (Resolve_Type_Spec (E1_T)) = DN_UNIVERSAL_INTEGER then
               T_A1 := Subst_Base_Type (Ctx, Res_Type);
            end if;
            if not Is_Void_Tree (E2_T) and then KIND (Resolve_Type_Spec (E2_T)) = DN_UNIVERSAL_INTEGER then
               T_A2 := Subst_Base_Type (Ctx, Res_Type);
            end if;
         end if;
      declare
         L   : constant Expr_Val := Emit_Expr (Ctx, A1_N, T_A1);
         R_V : constant Expr_Val := Emit_Expr (Ctx, A2_N, T_A2);
         LT  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, L.Ada_Type));
         PT  : constant String := To_String (L.Pir_Type);
      begin
         -- Structural equality / inequality on records or arrays (Op_Pos 7:=, 8:/=)
         if (Op_Pos = 7 or else Op_Pos = 8) and then (L.Kind = EK_Addr or else L.Kind = EK_Slice) then
            declare
               LTI : constant Type_Info := Get_Type_Info (Ctx, LT);
               Eq_Res : Unbounded_String;

               function Emit_Val_Equal
                 (P1_Reg, P2_Reg : String;
                  Val_T          : TREE;
                  Is_Dyn_Desc    : Boolean := False;
                  Depth          : Natural := 0;
                  Align_Override : Natural := 0) return String
               is
                  RT  : constant TREE := Subst_Resolve_Type (Ctx, Val_T);
                  VTI : constant Type_Info := Get_Type_Info (Ctx, RT);
               begin
                  if Depth > 32 or else VTI.Kind = TK_Void then
                     declare
                        Z : constant String := Next_Reg (Ctx.E, "feq1");
                     begin
                        Emit_Inst (Ctx.E, Z & " = const.i8 1");
                        return Z;
                     end;
                  end if;

                  if Is_Dyn_Desc then
                     -- Both P1_Reg and P2_Reg point to @__ada_arr_desc structs
                     declare
                        UV1, UV2 : Expr_Val;
                        ETI : constant Type_Info := Get_Type_Info (Ctx, VTI.Elem_Type);
                        ND  : constant Natural := Natural'Max (1, Natural'Min (VTI.NDims, Max_Array_Dims));
                        All_Eq : Unbounded_String;
                        Tot64  : Unbounded_String;
                        Tot2   : Unbounded_String;
                        L_C : constant String := Next_Label (Ctx.E, "deq_c");
                        L_H : constant String := Next_Label (Ctx.E, "deq_h");
                        L_B : constant String := Next_Label (Ctx.E, "deq_b");
                        L_E : constant String := Next_Label (Ctx.E, "deq_e");
                        IR  : constant String := Next_Reg (Ctx.E, "dei");
                        Dn  : constant String := Next_Reg (Ctx.E, "ded");
                        EEq : Unbounded_String;
                        Nx  : constant String := Next_Reg (Ctx.E, "denx");
                        OutR : constant String := Next_Reg (Ctx.E, "der");
                     begin
                        Emit_Unpack_Arr_Desc (Ctx, P1_Reg, RT, UV1);
                        Emit_Unpack_Arr_Desc (Ctx, P2_Reg, RT, UV2);
                        for D_I in 1 .. ND loop
                           declare
                              C1 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (UV1.First_Regs (D_I)), To_String (UV1.Last_Regs (D_I)));
                              C2 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (UV2.First_Regs (D_I)), To_String (UV2.Last_Regs (D_I)));
                              Eq : constant String := Next_Reg (Ctx.E, "ddeq");
                           begin
                              Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & C1 & ", " & C2);
                              if D_I = 1 then
                                 All_Eq := To_Unbounded_String (Eq);
                                 Tot64  := To_Unbounded_String (C1);
                                 Tot2   := To_Unbounded_String (C2);
                              else
                                 declare
                                    A2 : constant String := Next_Reg (Ctx.E, "dda");
                                    M2 : constant String := Next_Reg (Ctx.E, "ddm");
                                    N2 : constant String := Next_Reg (Ctx.E, "ddn");
                                 begin
                                    Emit_Inst (Ctx.E, A2 & " = and.i8 " & To_String (All_Eq) & ", " & Eq);
                                    Emit_Inst (Ctx.E, M2 & " = mul.i64 " & To_String (Tot64) & ", " & C1);
                                    Emit_Inst (Ctx.E, N2 & " = mul.i64 " & To_String (Tot2) & ", " & C2);
                                    All_Eq := To_Unbounded_String (A2);
                                    Tot64  := To_Unbounded_String (M2);
                                    Tot2   := To_Unbounded_String (N2);
                                 end;
                              end if;
                           end;
                        end loop;
                        -- LRM 4.5.2(7) (c45264b): two null arrays of the same type are always equal!
                        declare
                           Z1 : constant String := Next_Reg (Ctx.E, "dz1");
                           Z2 : constant String := Next_Reg (Ctx.E, "dz2");
                           BN : constant String := Next_Reg (Ctx.E, "dbn");
                        begin
                           Emit_Inst (Ctx.E, Z1 & " = cmp_eq.i64 " & To_String (Tot64) & ", 0");
                           Emit_Inst (Ctx.E, Z2 & " = cmp_eq.i64 " & To_String (Tot2) & ", 0");
                           Emit_Inst (Ctx.E, BN & " = and.i8 " & Z1 & ", " & Z2);
                           Emit_Term (Ctx.E, "br " & BN & ", " & L_E & "(1), " & L_C);
                           Emit_Label (Ctx.E, L_C);
                        end;
                        Emit_Term (Ctx.E, "br " & To_String (All_Eq) & ", " & L_H & "(0), " & L_E & "(0)");
                        Emit_Label (Ctx.E, L_H, IR & ": i64");
                        Emit_Inst (Ctx.E, Dn & " = cmp_ge_s.i64 " & IR & ", " & To_String (Tot64));
                        Emit_Term (Ctx.E, "br " & Dn & ", " & L_E & "(1), " & L_B);
                        Emit_Label (Ctx.E, L_B);
                        declare
                           EP1 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (UV1.Reg), IR);
                           EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (UV2.Reg), IR);
                        begin
                           EEq := To_Unbounded_String (Emit_Val_Equal (EP1, EP2, VTI.Elem_Type,
                             Is_Dyn_Desc => (ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic),
                             Depth       => Depth + 1));
                        end;
                        Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                        Emit_Term (Ctx.E, "br " & To_String (EEq) & ", " & L_H & "(" & Nx & "), " & L_E & "(0)");
                        Emit_Label (Ctx.E, L_E, OutR & ": i8");
                        return OutR;
                     end;
                  elsif VTI.Kind = TK_Scalar then
                     declare
                        V1 : constant String := Next_Reg (Ctx.E, "sv1");
                        V2 : constant String := Next_Reg (Ctx.E, "sv2");
                        Eq : constant String := Next_Reg (Ctx.E, "seq");
                        PT_S : constant String := To_String (VTI.Pir_Type);
                        LdAl : constant Natural :=
                          (if Align_Override > 0 then Align_Override else Natural'Max (1, VTI.Align));
                     begin
                        Emit_Inst (Ctx.E, V1 & " = load." & PT_S & " " & P1_Reg & ", align " & Img (LdAl));
                        Emit_Inst (Ctx.E, V2 & " = load." & PT_S & " " & P2_Reg & ", align " & Img (LdAl));
                        if PT_S = "f32" or else PT_S = "f64" then
                           Emit_Inst (Ctx.E, Eq & " = fcmp_oeq." & PT_S & " " & V1 & ", " & V2);
                        else
                           Emit_Inst (Ctx.E, Eq & " = cmp_eq." & PT_S & " " & V1 & ", " & V2);
                        end if;
                        return Eq;
                     end;
                  elsif VTI.Kind = TK_Constrained_Array and then not VTI.Is_Dynamic then
                     declare
                        ETI : constant Type_Info := Get_Type_Info (Ctx, VTI.Elem_Type);
                        N_Tot : Integer_64 := 1;
                     begin
                        for D_I in 1 .. VTI.NDims loop
                           if VTI.Dims (D_I).Last >= VTI.Dims (D_I).First then
                              N_Tot := N_Tot * (VTI.Dims (D_I).Last - VTI.Dims (D_I).First + 1);
                           else
                              N_Tot := 0;
                           end if;
                        end loop;
                        if N_Tot <= 0 then
                           declare
                              Z : constant String := Next_Reg (Ctx.E, "aeq1");
                           begin
                              Emit_Inst (Ctx.E, Z & " = const.i8 1");
                              return Z;
                           end;
                        end if;
                        declare
                           L_H : constant String := Next_Label (Ctx.E, "caq_h");
                           L_B : constant String := Next_Label (Ctx.E, "caq_b");
                           L_E : constant String := Next_Label (Ctx.E, "caq_e");
                           IR  : constant String := Next_Reg (Ctx.E, "cai");
                           Dn  : constant String := Next_Reg (Ctx.E, "cad");
                           EEq : Unbounded_String;
                           Nx  : constant String := Next_Reg (Ctx.E, "canx");
                           OutR : constant String := Next_Reg (Ctx.E, "car");
                        begin
                           Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                           Emit_Label (Ctx.E, L_H, IR & ": i64");
                           Emit_Inst (Ctx.E, Dn & " = cmp_ge_s.i64 " & IR & ", " & Img (Long_Long_Integer (N_Tot)));
                           Emit_Term (Ctx.E, "br " & Dn & ", " & L_E & "(1), " & L_B);
                           Emit_Label (Ctx.E, L_B);
                           declare
                              EP1 : constant String := Emit_Elem_Ptr (Ctx, ETI, P1_Reg, IR);
                              EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, P2_Reg, IR);
                           begin
                              EEq := To_Unbounded_String (Emit_Val_Equal (EP1, EP2, VTI.Elem_Type,
                                Is_Dyn_Desc => (ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic),
                                Depth       => Depth + 1));
                           end;
                           Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                           Emit_Term (Ctx.E, "br " & To_String (EEq) & ", " & L_H & "(" & Nx & "), " & L_E & "(0)");
                           Emit_Label (Ctx.E, L_E, OutR & ": i8");
                           return OutR;
                        end;
                     end;
                  elsif VTI.Kind = TK_Record then
                     declare
                        BR   : constant TREE := Subst_Base_Type (Ctx, RT);
                        DS   : constant TREE := (if not Is_Void_Tree (BR) and then KIND (BR) = DN_RECORD then D (SM_DISCRIMINANT_S, BR) else CONST_VOID);
                        CL   : constant TREE := (if not Is_Void_Tree (BR) and then KIND (BR) = DN_RECORD then D (SM_COMP_LIST, BR) else CONST_VOID);
                        ResA : constant String := Next_Reg (Ctx.E, "reqa");
                        L_Fail : constant String := Next_Label (Ctx.E, "req_f");
                        L_Done : constant String := Next_Label (Ctx.E, "req_d");
                        OutR   : constant String := Next_Reg (Ctx.E, "reqr");

                        procedure Emit_Rec_FP
                          (Rec_P : String;
                           FE    : Field_Entry;
                           Out_R : String)
                        is
                        begin
                           if not FE.In_Union and then not FE.Has_Explicit_Offset then
                              Emit_Inst (Ctx.E,
                                Out_R & " = field_ptr " & To_String (VTI.Pir_Type) & ", " &
                                Rec_P & ", " & Img (FE.Field_Idx));
                           else
                              declare
                                 Off_R : constant String := Next_Reg (Ctx.E, "rfo");
                              begin
                                 Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                                 Emit_Inst (Ctx.E, Out_R & " = ptr_add.i64 " & Rec_P & ", " & Off_R & " [inbounds]");
                              end;
                           end if;
                        end Emit_Rec_FP;

                        procedure Check_Field (CID : TREE) is
                           Can_C : constant TREE := Canonical_Def (CID);
                           CK    : constant Unsigned_32 := Tree_Key (Can_C);
                        begin
                           if not Ctx.Fields.Contains (CK) then return; end if;
                           declare
                              FE   : constant Field_Entry := Ctx.Fields.Element (CK);
                              CT   : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Can_C));
                              CTI  : constant Type_Info := Get_Type_Info (Ctx, CT);
                              FP1  : constant String := Next_Reg (Ctx.E, "rf1");
                              FP2  : constant String := Next_Reg (Ctx.E, "rf2");
                              FEq  : Unbounded_String;
                              L_Nx : constant String := Next_Label (Ctx.E, "req_n");
                           begin
                              if Is_Bitfield (FE) and then CTI.Kind = TK_Scalar then
                                 declare
                                    V1 : constant String := Emit_Bitfield_Load (Ctx, P1_Reg, FE, CT);
                                    V2 : constant String := Emit_Bitfield_Load (Ctx, P2_Reg, FE, CT);
                                    Eq : constant String := Next_Reg (Ctx.E, "bfeq");
                                    PT_F : constant String := To_String (CTI.Pir_Type);
                                 begin
                                    if PT_F = "f32" or else PT_F = "f64" then
                                       Emit_Inst (Ctx.E, Eq & " = fcmp_oeq." & PT_F & " " & V1 & ", " & V2);
                                    else
                                       Emit_Inst (Ctx.E, Eq & " = cmp_eq." & PT_F & " " & V1 & ", " & V2);
                                    end if;
                                    FEq := To_Unbounded_String (Eq);
                                 end;
                              else
                                 Emit_Rec_FP (P1_Reg, FE, FP1);
                                 Emit_Rec_FP (P2_Reg, FE, FP2);
                                 FEq := To_Unbounded_String (Emit_Val_Equal (FP1, FP2, CT,
                                   Is_Dyn_Desc    => (CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic),
                                   Depth          => Depth + 1,
                                   Align_Override => Explicit_Field_Align (FE, CTI.Align)));
                              end if;
                              Emit_Term (Ctx.E, "br " & To_String (FEq) & ", " & L_Nx & ", " & L_Fail);
                              Emit_Label (Ctx.E, L_Nx);
                           end;
                        end Check_Field;

                        procedure Check_Comp_List (CList : TREE) is
                           VP : TREE;
                        begin
                           if Is_Void_Tree (CList) or else KIND (CList) /= DN_COMP_LIST then return; end if;
                           if not Is_Void_Tree (D (AS_DECL_S, CList)) then
                              declare
                                 DL : SEQ_TYPE := LIST (D (AS_DECL_S, CList));
                                 DI : TREE;
                              begin
                                 while not IS_EMPTY (DL) loop
                                    POP (DL, DI);
                                    if KIND (DI) = DN_VARIABLE_DECL then
                                       declare
                                          NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DI));
                                          CI : TREE;
                                       begin
                                          while not IS_EMPTY (NS) loop
                                             POP (NS, CI);
                                             Check_Field (CI);
                                          end loop;
                                       end;
                                    end if;
                                 end loop;
                              end;
                           end if;
                           VP := D (AS_VARIANT_PART, CList);
                           if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART then
                              declare
                                 D_Name : constant TREE := D (AS_NAME, VP);
                                 D_Def  : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D_Name)));
                                 DK     : constant Unsigned_32 := Tree_Key (D_Def);
                              begin
                                 if Ctx.Fields.Contains (DK) and then not Is_Void_Tree (D (AS_VARIANT_S, VP)) then
                                    declare
                                       DFE  : constant Field_Entry := Ctx.Fields.Element (DK);
                                       DT_A : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, D_Def));
                                       DTI  : constant Type_Info := Get_Type_Info (Ctx, DT_A);
                                       DV64 : constant String := Emit_Load_Field_I64 (Ctx, P1_Reg, To_String (VTI.Pir_Type), DFE, DT_A, DTI);
                                       VL   : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                                       VN   : TREE;
                                       L_VEnd : constant String := Next_Label (Ctx.E, "vpe");
                                    begin
                                       while not IS_EMPTY (VL) loop
                                          POP (VL, VN);
                                          if KIND (VN) = DN_VARIANT then
                                             declare
                                                Has_Others : Boolean := False;
                                                Hit        : constant String :=
                                                  Emit_Variant_Choice_Match (Ctx, DV64, DT_A, D (AS_CHOICE_S, VN), Has_Others);
                                                L_Hit      : constant String := Next_Label (Ctx.E, "vh");
                                                L_Nxt      : constant String := Next_Label (Ctx.E, "vn");
                                             begin
                                                if Has_Others then
                                                   Check_Comp_List (D (AS_COMP_LIST, VN));
                                                   Emit_Term (Ctx.E, "jmp " & L_VEnd);
                                                   Emit_Label (Ctx.E, L_Nxt);
                                                elsif Hit'Length > 0 then
                                                   Emit_Term (Ctx.E, "br " & Hit & ", " & L_Hit & ", " & L_Nxt);
                                                   Emit_Label (Ctx.E, L_Hit);
                                                   Check_Comp_List (D (AS_COMP_LIST, VN));
                                                   Emit_Term (Ctx.E, "jmp " & L_VEnd);
                                                   Emit_Label (Ctx.E, L_Nxt);
                                                end if;
                                             end;
                                          end if;
                                       end loop;
                                       Emit_Term (Ctx.E, "jmp " & L_VEnd);
                                       Emit_Label (Ctx.E, L_VEnd);
                                    end;
                                 end if;
                              end;
                           end if;
                        end Check_Comp_List;
                     begin
                        Emit_Entry_Alloca (Ctx.E, ResA & " = alloca i8, align 1");
                        Emit_Inst (Ctx.E, "store.i8 1, " & ResA & ", align 1");
                        if not Is_Void_Tree (DS) then
                           declare
                              DL : SEQ_TYPE := LIST (DS);
                              DD : TREE;
                           begin
                              while not IS_EMPTY (DL) loop
                                 POP (DL, DD);
                                 declare
                                    NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                    DID : TREE;
                                 begin
                                    while not IS_EMPTY (NS) loop
                                       POP (NS, DID);
                                       Check_Field (DID);
                                    end loop;
                                 end;
                              end loop;
                           end;
                        end if;
                        Check_Comp_List (CL);
                        Emit_Term (Ctx.E, "jmp " & L_Done);
                        Emit_Label (Ctx.E, L_Fail);
                        Emit_Inst (Ctx.E, "store.i8 0, " & ResA & ", align 1");
                        Emit_Term (Ctx.E, "jmp " & L_Done);
                        Emit_Label (Ctx.E, L_Done);
                        Emit_Inst (Ctx.E, OutR & " = load.i8 " & ResA & ", align 1");
                        return OutR;
                     end;
                  else
                     declare
                        Z : constant String := Next_Reg (Ctx.E, "feq1");
                     begin
                        Emit_Inst (Ctx.E, Z & " = const.i8 1");
                        return Z;
                     end;
                  end if;
               end Emit_Val_Equal;
            begin
               if LTI.Kind = TK_Record then
                  -- Null record (0 discriminants & 0 components): always equal (LRM 4.5.2, c37411a)!
                  declare
                     Is_Null_Rec : Boolean := False;
                  begin
                     declare
                        BR : constant TREE := Subst_Base_Type (Ctx, LT);
                        DS : constant TREE := (if not Is_Void_Tree (BR) and then KIND (BR) = DN_RECORD then D (SM_DISCRIMINANT_S, BR) else CONST_VOID);
                        CL : constant TREE := (if not Is_Void_Tree (BR) and then KIND (BR) = DN_RECORD then D (SM_COMP_LIST, BR) else CONST_VOID);
                        Has_D : constant Boolean := not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS));
                        Has_C : constant Boolean := not Is_Void_Tree (CL) and then
                          ((not Is_Void_Tree (D (AS_DECL_S, CL)) and then not IS_EMPTY (LIST (D (AS_DECL_S, CL))))
                           or else not Is_Void_Tree (D (AS_VARIANT_PART, CL)));
                     begin
                        Is_Null_Rec := not Has_D and then not Has_C;
                     end;
                     if Is_Null_Rec then
                        declare
                           OutR : constant String := Next_Reg (Ctx.E, "neq");
                        begin
                           Emit_Inst (Ctx.E, OutR & " = const.i8 1");
                           Eq_Res := To_Unbounded_String (OutR);
                        end;
                     else
                        Eq_Res := To_Unbounded_String (Emit_Val_Equal (To_String (L.Reg), To_String (R_V.Reg), LT));
                     end if;
                  end;
               else
                  -- All 1D..4D arrays (constrained or unconstrained): check all dim lengths match first (c45264c), then compare!
                  declare
                     ETI : constant Type_Info := Get_Type_Info (Ctx, LTI.Elem_Type);
                     Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
                     EAl : constant Natural := (if Is_DE then 8 else Natural'Max (1, ETI.Align));
                     ESz : constant Natural := Elem_Storage_Size (ETI);
                     ND  : constant Natural := Natural'Max (1, Natural'Min (LTI.NDims, Max_Array_Dims));
                     All_Eq : Unbounded_String;
                     Tot64  : Unbounded_String;
                     Tot2   : Unbounded_String;
                     Byt : constant String := Next_Reg (Ctx.E, "byt");
                     L_Chk : constant String := Next_Label (Ctx.E, "aeq_c");
                     L_Hdr : constant String := Next_Label (Ctx.E, "aeq_h");
                     L_Bdy : constant String := Next_Label (Ctx.E, "aeq_b");
                     L_End : constant String := Next_Label (Ctx.E, "aeq_e");
                     I_Reg : constant String := Next_Reg (Ctx.E, "ai");
                     Done  : constant String := Next_Reg (Ctx.E, "ad");
                     Q1    : constant String := Next_Reg (Ctx.E, "ap1");
                     Q2    : constant String := Next_Reg (Ctx.E, "ap2");
                     B1    : constant String := Next_Reg (Ctx.E, "ab1");
                     B2    : constant String := Next_Reg (Ctx.E, "ab2");
                     Neq   : constant String := Next_Reg (Ctx.E, "an");
                     Nx    : constant String := Next_Reg (Ctx.E, "anx");
                     OutR  : constant String := Next_Reg (Ctx.E, "ar");
                  begin
                     for D_I in 1 .. ND loop
                        declare
                           F1, L1, F2, L2 : Unbounded_String;
                           C1 : Unbounded_String;
                           C2 : Unbounded_String;
                           Eq : constant String := Next_Reg (Ctx.E, "deq");
                        begin
                           Get_Array_Dim_Bounds (Ctx, L, D_I, F1, L1);
                           Get_Array_Dim_Bounds (Ctx, R_V, D_I, F2, L2);
                           C1 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F1), To_String (L1)));
                           C2 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F2), To_String (L2)));
                           Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & To_String (C1) & ", " & To_String (C2));
                           if D_I = 1 then
                              All_Eq := To_Unbounded_String (Eq);
                              Tot64  := C1;
                              Tot2   := C2;
                           else
                              declare
                                 A2 : constant String := Next_Reg (Ctx.E, "ae2");
                                 M2 : constant String := Next_Reg (Ctx.E, "am2");
                                 N2 : constant String := Next_Reg (Ctx.E, "an2");
                              begin
                                 Emit_Inst (Ctx.E, A2 & " = and.i8 " & To_String (All_Eq) & ", " & Eq);
                                 Emit_Inst (Ctx.E, M2 & " = mul.i64 " & To_String (Tot64) & ", " & To_String (C1));
                                 Emit_Inst (Ctx.E, N2 & " = mul.i64 " & To_String (Tot2) & ", " & To_String (C2));
                                 All_Eq := To_Unbounded_String (A2);
                                 Tot64  := To_Unbounded_String (M2);
                                 Tot2   := To_Unbounded_String (N2);
                              end;
                           end if;
                        end;
                     end loop;
                     -- LRM 4.5.2(7) (c45264a): two null arrays of the same type are always equal even if dim lengths differ!
                     declare
                        Z1 : constant String := Next_Reg (Ctx.E, "az1");
                        Z2 : constant String := Next_Reg (Ctx.E, "az2");
                        BN : constant String := Next_Reg (Ctx.E, "abn");
                     begin
                        Emit_Inst (Ctx.E, Z1 & " = cmp_eq.i64 " & To_String (Tot64) & ", 0");
                        Emit_Inst (Ctx.E, Z2 & " = cmp_eq.i64 " & To_String (Tot2) & ", 0");
                        Emit_Inst (Ctx.E, BN & " = and.i8 " & Z1 & ", " & Z2);
                        Emit_Term (Ctx.E, "br " & BN & ", " & L_End & "(1), " & L_Chk);
                        Emit_Label (Ctx.E, L_Chk);
                     end;
                     if ETI.Kind in TK_Record | TK_Constrained_Array
                       or else Is_Float_Type (Subst_Resolve_Type (Ctx, LTI.Elem_Type))
                     then
                        Emit_Term (Ctx.E, "br " & To_String (All_Eq) & ", " & L_Hdr & "(0), " & L_End & "(0)");
                        Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                        Emit_Inst (Ctx.E, Done & " = cmp_ge_s.i64 " & I_Reg & ", " & To_String (Tot64));
                        Emit_Term (Ctx.E, "br " & Done & ", " & L_End & "(1), " & L_Bdy);
                        Emit_Label (Ctx.E, L_Bdy);
                        declare
                           EP1 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (L.Reg), I_Reg);
                           EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (R_V.Reg), I_Reg);
                           EEq : constant String := Emit_Val_Equal (EP1, EP2, LTI.Elem_Type, Is_Dyn_Desc => Is_DE, Depth => 1);
                        begin
                           Emit_Inst (Ctx.E, Nx & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                           Emit_Term (Ctx.E, "br " & EEq & ", " & L_Hdr & "(" & Nx & "), " & L_End & "(0)");
                        end;
                        Emit_Label (Ctx.E, L_End, OutR & ": i8");
                        Eq_Res := To_Unbounded_String (OutR);
                     else
                        Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                        Emit_Term (Ctx.E, "br " & To_String (All_Eq) & ", " & L_Hdr & "(0), " & L_End & "(0)");
                        Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                        Emit_Inst (Ctx.E, Done & " = cmp_ge_s.i64 " & I_Reg & ", " & Byt);
                        Emit_Term (Ctx.E, "br " & Done & ", " & L_End & "(1), " & L_Bdy);
                        Emit_Label (Ctx.E, L_Bdy);
                        Emit_Inst (Ctx.E, Q1 & " = ptr_add.i64 " & To_String (L.Reg) & ", " & I_Reg & " [inbounds]");
                        Emit_Inst (Ctx.E, Q2 & " = ptr_add.i64 " & To_String (R_V.Reg) & ", " & I_Reg & " [inbounds]");
                        Emit_Inst (Ctx.E, B1 & " = load.i8 " & Q1 & ", align 1");
                        Emit_Inst (Ctx.E, B2 & " = load.i8 " & Q2 & ", align 1");
                        Emit_Inst (Ctx.E, Neq & " = cmp_ne.i8 " & B1 & ", " & B2);
                        Emit_Inst (Ctx.E, Nx & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                        Emit_Term (Ctx.E, "br " & Neq & ", " & L_End & "(0), " & L_Hdr & "(" & Nx & ")");
                        Emit_Label (Ctx.E, L_End, OutR & ": i8");
                        Eq_Res := To_Unbounded_String (OutR);
                     end if;
                  end;
               end if;

               if Op_Pos = 8 then
                  declare
                     Inv : constant String := Next_Reg (Ctx.E, "neq");
                  begin
                     Emit_Inst (Ctx.E, Inv & " = xor.i8 " & To_String (Eq_Res) & ", 1");
                     Res.Reg := To_Unbounded_String (Inv);
                  end;
               else
                  Res.Reg := Eq_Res;
               end if;
               Res.Pir_Type := To_Unbounded_String ("i8");
               return Res;
            end;
         end if;

         -- 1D Boolean array logical ops (AND=0, OR=1, XOR=2)
         if Op_Pos in 0 .. 2 and then (Is_Array_Type (LT) or else Is_Array_Type (Res_Type)) then
            declare
               F1, L1, F2, L2 : Unbounded_String;
               D1   : constant String := Next_Reg (Ctx.E, "ad1");
               N1   : constant String := Next_Reg (Ctx.E, "an1");
               P1   : constant String := Next_Reg (Ctx.E, "ap1");
               C1   : constant String := Next_Reg (Ctx.E, "ac1");
               D2   : constant String := Next_Reg (Ctx.E, "ad2");
               N2   : constant String := Next_Reg (Ctx.E, "an2");
               P2   : constant String := Next_Reg (Ctx.E, "ap2");
               C2   : constant String := Next_Reg (Ctx.E, "ac2");
               Lne  : constant String := Next_Reg (Ctx.E, "alne");
               N64  : constant String := Next_Reg (Ctx.E, "an64");
               Buf  : Unbounded_String;
               L_H  : constant String := Next_Label (Ctx.E, "al_h");
               L_B  : constant String := Next_Label (Ctx.E, "al_b");
               L_E  : constant String := Next_Label (Ctx.E, "al_e");
               IR   : constant String := Next_Reg (Ctx.E, "ali");
               Cnd  : constant String := Next_Reg (Ctx.E, "alc");
               Q1   : constant String := Next_Reg (Ctx.E, "aq1");
               Q2   : constant String := Next_Reg (Ctx.E, "aq2");
               QD   : constant String := Next_Reg (Ctx.E, "aqd");
               B1   : constant String := Next_Reg (Ctx.E, "ab1");
               B2   : constant String := Next_Reg (Ctx.E, "ab2");
               BO   : constant String := Next_Reg (Ctx.E, "abo");
               Nx   : constant String := Next_Reg (Ctx.E, "anx");
               Opc  : constant String := (if Op_Pos = 0 then "and.i8" elsif Op_Pos = 1 then "or.i8" else "xor.i8");
            begin
               Get_Array_Dim_Bounds (Ctx, L, 1, F1, L1);
               Get_Array_Dim_Bounds (Ctx, R_V, 1, F2, L2);
               declare
                  BT    : constant TREE := Base_Type_Spec (LT);
                  CompT : constant TREE := (if not Is_Void_Tree (BT) and then KIND (BT) in DN_ARRAY | DN_CONSTRAINED_ARRAY
                                            then Subst_Tree (Ctx, D (SM_COMP_TYPE, BT)) else CONST_VOID);
                  ETI   : constant Type_Info := Get_Type_Info (Ctx, CompT);
                  EP    : constant String := (if not Is_Void_Tree (CompT) then To_String (ETI.Pir_Type) else "i8");
                  EAl   : constant Natural := (if not Is_Void_Tree (CompT) then Natural'Max (1, ETI.Align) else 1);
                  ESz   : constant Natural := (if not Is_Void_Tree (CompT) then Natural'Max (1, ETI.Byte_Size) else 1);
                  BOpc  : constant String := (if Op_Pos = 0 then "and." & EP elsif Op_Pos = 1 then "or." & EP else "xor." & EP);
                  Len1  : constant String := Emit_Dim_Len_I64 (Ctx, To_String (F1), To_String (L1));
                  Len2  : constant String := Emit_Dim_Len_I64 (Ctx, To_String (F2), To_String (L2));
                  Byt64 : constant String := Next_Reg (Ctx.E, "aby");
                  Off64 : constant String := Next_Reg (Ctx.E, "aof");
               begin
                  if Ctx.Enable_Checks then
                     Emit_Inst (Ctx.E, Lne & " = cmp_ne.i64 " & Len1 & ", " & Len2);
                     Emit_Check_Fail_If (Ctx, Lne);
                  end if;
                  Emit_Inst (Ctx.E, Byt64 & " = mul.i64 " & Len1 & ", " & Img (ESz));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Byt64 & ", " & Img (EAl), "ptr", Buf);
                  Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                  Emit_Label (Ctx.E, L_H, IR & ": i64");
                  Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Len1);
                  Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                  Emit_Label (Ctx.E, L_B);
                  Emit_Inst (Ctx.E, Off64 & " = mul.i64 " & IR & ", " & Img (ESz));
                  Emit_Inst (Ctx.E, Q1 & " = ptr_add.i64 " & To_String (L.Reg) & ", " & Off64 & " [inbounds]");
                  Emit_Inst (Ctx.E, Q2 & " = ptr_add.i64 " & To_String (R_V.Reg) & ", " & Off64 & " [inbounds]");
                  Emit_Inst (Ctx.E, QD & " = ptr_add.i64 " & To_String (Buf) & ", " & Off64 & " [inbounds]");
                  Emit_Inst (Ctx.E, B1 & " = load." & EP & " " & Q1 & ", align " & Img (EAl));
                  Emit_Inst (Ctx.E, B2 & " = load." & EP & " " & Q2 & ", align " & Img (EAl));
                  Emit_Inst (Ctx.E, BO & " = " & BOpc & " " & B1 & ", " & B2);
                  if not Is_Void_Tree (CompT) then
                     Emit_Range_Check (Ctx, BO, EP, CompT);
                  end if;
                  Emit_Inst (Ctx.E, "store." & EP & " " & BO & ", " & QD & ", align " & Img (EAl));
                  Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                  Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                  Emit_Label (Ctx.E, L_E);
                  Res.Kind           := EK_Slice;
                  Res.Reg            := Buf;
                  Res.Pir_Type       := To_Unbounded_String ("ptr");
                  Res.Ada_Type       := Res_Type;
                  Res.Align          := EAl;
                  Res.NDims          := 1;
                  Res.First_Regs (1) := F1;
                  Res.Last_Regs (1)  := L1;
                  return Res;
               end;
            end;
         end if;

         -- 1D discrete array lexicographical ordering (<, <=, >, >=)
         if Op_Pos in 9 .. 12 and then Is_Array_Type (LT) then
            declare
               BT    : constant TREE := Base_Type_Spec (LT);
               ET    : constant TREE := Subst_Tree (Ctx, D (SM_COMP_TYPE, BT));
               ETI   : constant Type_Info := Get_Type_Info (Ctx, ET);
               EP    : constant String := To_String (ETI.Pir_Type);
               F1, L1, F2, L2 : Unbounded_String;
               D1    : constant String := Next_Reg (Ctx.E, "rd1");
               N1    : constant String := Next_Reg (Ctx.E, "rn1");
               P1    : constant String := Next_Reg (Ctx.E, "rp1");
               C1    : constant String := Next_Reg (Ctx.E, "rc1");
               D2    : constant String := Next_Reg (Ctx.E, "rd2");
               N2    : constant String := Next_Reg (Ctx.E, "rn2");
               P2    : constant String := Next_Reg (Ctx.E, "rp2");
               C2    : constant String := Next_Reg (Ctx.E, "rc2");
               Mlt   : constant String := Next_Reg (Ctx.E, "rmlt");
               MinL  : constant String := Next_Reg (Ctx.E, "rmin");
               Min64 : constant String := Next_Reg (Ctx.E, "rm64");
               L_H   : constant String := Next_Label (Ctx.E, "ar_lh");
               L_B   : constant String := Next_Label (Ctx.E, "ar_lb");
               L_D   : constant String := Next_Label (Ctx.E, "ar_ld");
               L_E   : constant String := Next_Label (Ctx.E, "ar_le");
               IR    : constant String := Next_Reg (Ctx.E, "ari");
               Done  : constant String := Next_Reg (Ctx.E, "ard");
               Q1    : constant String := Next_Reg (Ctx.E, "aq1");
               Q2    : constant String := Next_Reg (Ctx.E, "aq2");
               V1    : constant String := Next_Reg (Ctx.E, "av1");
               V2    : constant String := Next_Reg (Ctx.E, "av2");
               Neq   : constant String := Next_Reg (Ctx.E, "ane");
               ECmp  : constant String := Next_Reg (Ctx.E, "aec");
               LCmp  : constant String := Next_Reg (Ctx.E, "alc");
               Nx    : constant String := Next_Reg (Ctx.E, "anx");
               OutR  : constant String := Next_Reg (Ctx.E, "aor");
               CmpOp : constant String :=
                 (if Op_Pos = 9 then "cmp_lt_s"
                  elsif Op_Pos = 10 then "cmp_le_s"
                  elsif Op_Pos = 11 then "cmp_gt_s"
                  else "cmp_ge_s");
               E_Uns : constant Boolean := Is_Unsigned_Scalar (ET, EP);
               ECmpOp : constant String :=
                 (if E_Uns then
                    (if Op_Pos = 9 then "cmp_lt_u"
                     elsif Op_Pos = 10 then "cmp_le_u"
                     elsif Op_Pos = 11 then "cmp_gt_u"
                     else "cmp_ge_u")
                  else CmpOp);
            begin
               Get_Array_Dim_Bounds (Ctx, L, 1, F1, L1);
               Get_Array_Dim_Bounds (Ctx, R_V, 1, F2, L2);
               declare
                  Len1 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (F1), To_String (L1));
                  Len2 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (F2), To_String (L2));
               begin
                  Emit_Inst (Ctx.E, Mlt & " = cmp_lt_s.i64 " & Len1 & ", " & Len2);
                  Emit_Inst (Ctx.E, Min64 & " = select.i64 " & Mlt & ", " & Len1 & ", " & Len2);
                  Emit_Inst (Ctx.E, LCmp & " = " & CmpOp & ".i64 " & Len1 & ", " & Len2);
               end;
               Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
               Emit_Label (Ctx.E, L_H, IR & ": i64");
               Emit_Inst (Ctx.E, Done & " = cmp_ge_s.i64 " & IR & ", " & Min64);
               Emit_Term (Ctx.E, "br " & Done & ", " & L_E & "(" & LCmp & "), " & L_B);
               Emit_Label (Ctx.E, L_B);
               Emit_Inst (Ctx.E, Q1 & " = elem_ptr " & EP & ", " & To_String (L.Reg) & ", " & IR);
               Emit_Inst (Ctx.E, Q2 & " = elem_ptr " & EP & ", " & To_String (R_V.Reg) & ", " & IR);
               Emit_Inst (Ctx.E, V1 & " = load." & EP & " " & Q1 & ", align " & Img (ETI.Align));
               Emit_Inst (Ctx.E, V2 & " = load." & EP & " " & Q2 & ", align " & Img (ETI.Align));
               Emit_Inst (Ctx.E, Neq & " = cmp_ne." & EP & " " & V1 & ", " & V2);
               Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
               Emit_Term (Ctx.E, "br " & Neq & ", " & L_D & ", " & L_H & "(" & Nx & ")");
               Emit_Label (Ctx.E, L_D);
               Emit_Inst (Ctx.E, ECmp & " = " & ECmpOp & "." & EP & " " & V1 & ", " & V2);
               Emit_Term (Ctx.E, "jmp " & L_E & "(" & ECmp & ")");
               Emit_Label (Ctx.E, L_E, OutR & ": i8");
               Res.Kind     := EK_Scalar;
               Res.Reg      := To_Unbounded_String (OutR);
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Res_Type;
               Res.Align    := 1;
               return Res;
            end;
         end if;

         -- Fixed-point * Fixed-point and Fixed-point / Fixed-point -> universal_fixed (f64)
         if (Op_Pos = 15 or else Op_Pos = 16)
           and then Is_Fixed_Type (LT) and then Is_Fixed_Type (R_V.Ada_Type)
         then
            declare
               LF   : constant String := Next_Reg (Ctx.E, "fl");
               LS   : constant String := Next_Reg (Ctx.E, "fls");
               RF   : constant String := Next_Reg (Ctx.E, "fr");
               RS   : constant String := Next_Reg (Ctx.E, "frs");
               OReg : constant String := Next_Reg (Ctx.E, "uf");
            begin
               Emit_Inst (Ctx.E, LF & " = sitofp.f64 " & To_String (L.Reg));
               Emit_Inst (Ctx.E, LS & " = fmul.f64 " & LF & ", " & Format_Float64 (Fixed_Small (LT)));
               Emit_Inst (Ctx.E, RF & " = sitofp.f64 " & To_String (R_V.Reg));
               Emit_Inst (Ctx.E, RS & " = fmul.f64 " & RF & ", " & Format_Float64 (Fixed_Small (R_V.Ada_Type)));
               if Op_Pos = 15 then
                  Emit_Inst (Ctx.E, OReg & " = fmul.f64 " & LS & ", " & RS);
               else
                  if Ctx.Enable_Checks then
                     declare
                        RZ : constant String := Next_Reg (Ctx.E, "fxdz");
                     begin
                        Emit_Inst (Ctx.E, RZ & " = cmp_eq." & To_String (R_V.Pir_Type) & " " & To_String (R_V.Reg) & ", 0");
                        Emit_Check_Fail_If (Ctx, RZ);
                     end;
                  end if;
                  Emit_Inst (Ctx.E, OReg & " = fdiv.f64 " & LS & ", " & RS);
               end if;
               Res.Kind     := EK_Scalar;
               Res.Reg      := To_Unbounded_String (OReg);
               Res.Pir_Type := To_Unbounded_String ("f64");
               Res.Ada_Type := Res_Type;
               Res.Align    := 8;
               return Res;
            end;
         end if;

         -- Coerce operands cleanly (including fixed-point vs universal_real, Integer * Fixed, and float vs int)
         declare
            L_Reg   : Unbounded_String := L.Reg;
            R_Reg   : Unbounded_String := R_V.Reg;
            Op_PT   : Unbounded_String := To_Unbounded_String (PT);
            Is_Real : Boolean := (PT = "f32" or else PT = "f64");
            Out_R   : constant String := Next_Reg (Ctx.E, "b");
         begin
            if Op_Pos = 15 and then Is_Fixed_Type (R_V.Ada_Type) and then not Is_Fixed_Type (LT) then
               Op_PT   := R_V.Pir_Type;
               Is_Real := False;
               L_Reg   := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (L_Reg), PT, To_String (Op_PT)));
            elsif Is_Fixed_Type (LT) and then (To_String (R_V.Pir_Type) = "f32" or else To_String (R_V.Pir_Type) = "f64") then
               R_Reg := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (R_Reg), To_String (R_V.Pir_Type), LT, PT));
            elsif Is_Fixed_Type (R_V.Ada_Type) and then Is_Real then
               Op_PT   := R_V.Pir_Type;
               Is_Real := False;
               L_Reg   := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (L_Reg), PT, R_V.Ada_Type, To_String (Op_PT)));
            elsif Op_Pos /= 20 and then To_String (R_V.Pir_Type) /= PT then
               if not Is_Real and then (To_String (R_V.Pir_Type) = "f32" or else To_String (R_V.Pir_Type) = "f64") then
                  Op_PT   := R_V.Pir_Type;
                  Is_Real := True;
                  L_Reg   := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (L_Reg), PT, To_String (Op_PT),
                               Is_Unsigned => Is_Unsigned_Scalar (LT, PT)));
               elsif not Is_Real and then PT /= "ptr" and then To_String (R_V.Pir_Type) /= "ptr"
                 and then Int_Bits_Of_Pir (To_String (R_V.Pir_Type)) > Int_Bits_Of_Pir (PT)
               then
                  Op_PT := R_V.Pir_Type;
                  L_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L_Reg), PT, To_String (Op_PT),
                             Is_Unsigned => Is_Unsigned_Scalar (LT, PT)));
               else
                  R_Reg := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (R_Reg), To_String (R_V.Pir_Type), PT,
                             Is_Unsigned => Is_Unsigned_Scalar (R_V.Ada_Type, To_String (R_V.Pir_Type))));
               end if;
            end if;

            -- P3-1.3: Widen 'Size-packed integer operands (i8/i16) to T'Base's signed arithmetic width
            -- so Byte(120)+Byte(20), Byte(200)/2, Small(100)+Small(100)-150, etc. use T'Base semantics!
            if not Is_Real and then To_String (Op_PT) in "i8" | "i16"
              and then Op_Pos in 7 .. 20
              and then (Is_Integer_Type (LT) or else Is_Integer_Type (R_V.Ada_Type))
            then
               declare
                  Cur_PT : constant String := To_String (Op_PT);
                  W_PT   : Unbounded_String := Op_PT;
                  Base_T : constant TREE :=
                    (if Is_Integer_Type (LT) then Base_Type_Spec (Subst_Tree (Ctx, LT))
                     else Base_Type_Spec (Subst_Tree (Ctx, R_V.Ada_Type)));
                  St_B   : Boolean;
                  BLo, BHi : Integer_64;
               begin
                  Get_Discrete_Bounds (Base_T, St_B, BLo, BHi);
                  if not St_B or else BLo < -32768 or else BHi > 32767 then
                     W_PT := To_Unbounded_String ("i32");
                  elsif Cur_PT = "i8" then
                     W_PT := To_Unbounded_String ("i16");
                  end if;
                  if (Is_Unsigned_Scalar (LT, Cur_PT) or else Is_Unsigned_Scalar (R_V.Ada_Type, Cur_PT))
                    and then To_String (W_PT) = Cur_PT
                  then
                     W_PT := To_Unbounded_String (if Cur_PT = "i8" then "i16" else "i32");
                  end if;
                  if To_String (W_PT) /= Cur_PT then
                     L_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L_Reg), Cur_PT, To_String (W_PT),
                                Is_Unsigned => Is_Unsigned_Scalar (LT, Cur_PT)));
                     if Op_Pos /= 20 then
                        R_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (R_Reg), Cur_PT, To_String (W_PT),
                                   Is_Unsigned => Is_Unsigned_Scalar (R_V.Ada_Type, Cur_PT)));
                     end if;
                     Op_PT := W_PT;
                  end if;
               end;
            end if;

            declare
               Use_PT : constant String := To_String (Op_PT);
               Use_L  : constant String := To_String (L_Reg);
               Use_R  : constant String := To_String (R_Reg);
            begin
               case Op_Pos is
                  when 0 => -- AND
                     Emit_Inst (Ctx.E, Out_R & " = and." & Use_PT & " " & Use_L & ", " & Use_R);
                  when 1 => -- OR
                     Emit_Inst (Ctx.E, Out_R & " = or." & Use_PT & " " & Use_L & ", " & Use_R);
                  when 2 => -- XOR
                     Emit_Inst (Ctx.E, Out_R & " = xor." & Use_PT & " " & Use_L & ", " & Use_R);
                  when 7 => -- =
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_oeq." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_eq." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 8 => -- /=
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_une." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_ne." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 9 => -- <
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_olt." & Use_PT & " " & Use_L & ", " & Use_R);
                     elsif Is_Unsigned_Scalar (LT, Use_PT) then
                        Emit_Inst (Ctx.E, Out_R & " = cmp_lt_u." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_lt_s." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 10 => -- <=
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_ole." & Use_PT & " " & Use_L & ", " & Use_R);
                     elsif Is_Unsigned_Scalar (LT, Use_PT) then
                        Emit_Inst (Ctx.E, Out_R & " = cmp_le_u." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_le_s." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 11 => -- >
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_ogt." & Use_PT & " " & Use_L & ", " & Use_R);
                     elsif Is_Unsigned_Scalar (LT, Use_PT) then
                        Emit_Inst (Ctx.E, Out_R & " = cmp_gt_u." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_gt_s." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 12 => -- >=
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_oge." & Use_PT & " " & Use_L & ", " & Use_R);
                     elsif Is_Unsigned_Scalar (LT, Use_PT) then
                        Emit_Inst (Ctx.E, Out_R & " = cmp_ge_u." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_ge_s." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 13 => -- +
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fadd." & Use_PT & " " & Use_L & ", " & Use_R);
                     elsif Ctx.Enable_Checks then
                        declare
                           Ovf : constant String := Next_Reg (Ctx.E, "ov");
                        begin
                           Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = add_ovf_s." & Use_PT & " " & Use_L & ", " & Use_R);
                           Emit_Check_Fail_If (Ctx, Ovf);
                        end;
                     else
                        Emit_Inst (Ctx.E, Out_R & " = add." & Use_PT & " " & Use_L & ", " & Use_R & " [unsafe_s]");
                     end if;
                  when 14 => -- -
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fsub." & Use_PT & " " & Use_L & ", " & Use_R);
                     elsif Ctx.Enable_Checks then
                        declare
                           Ovf : constant String := Next_Reg (Ctx.E, "ov");
                        begin
                           Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = sub_ovf_s." & Use_PT & " " & Use_L & ", " & Use_R);
                           Emit_Check_Fail_If (Ctx, Ovf);
                        end;
                     else
                        Emit_Inst (Ctx.E, Out_R & " = sub." & Use_PT & " " & Use_L & ", " & Use_R & " [unsafe_s]");
                     end if;
                  when 15 => -- *
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fmul." & Use_PT & " " & Use_L & ", " & Use_R);
                     elsif Ctx.Enable_Checks then
                        declare
                           Ovf : constant String := Next_Reg (Ctx.E, "ov");
                        begin
                           Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = mul_ovf_s." & Use_PT & " " & Use_L & ", " & Use_R);
                           Emit_Check_Fail_If (Ctx, Ovf);
                        end;
                     else
                        Emit_Inst (Ctx.E, Out_R & " = mul." & Use_PT & " " & Use_L & ", " & Use_R & " [unsafe_s]");
                     end if;
                  when 16 => -- /
                     if Is_Real then
                        if Ctx.Enable_Checks then
                           declare
                              RZ : constant String := Next_Reg (Ctx.E, "fdz");
                           begin
                              Emit_Inst (Ctx.E, RZ & " = fcmp_oeq." & Use_PT & " " & Use_R & ", 0.0");
                              Emit_Check_Fail_If (Ctx, RZ);
                           end;
                        end if;
                        Emit_Inst (Ctx.E, Out_R & " = fdiv." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        if Ctx.Enable_Checks then
                           declare
                              Z   : constant String := Next_Reg (Ctx.E, "dz");
                              Im  : constant String := Next_Reg (Ctx.E, "dm");
                              M1  : constant String := Next_Reg (Ctx.E, "dm1");
                              Ov  : constant String := Next_Reg (Ctx.E, "dov");
                              Bad : constant String := Next_Reg (Ctx.E, "dbad");
                           begin
                              Emit_Inst (Ctx.E, Z & " = cmp_eq." & Use_PT & " " & Use_R & ", 0");
                              Emit_Inst (Ctx.E, Im & " = cmp_eq." & Use_PT & " " & Use_L & ", " & Img (Min_Signed_For_Pir (Use_PT)));
                              Emit_Inst (Ctx.E, M1 & " = cmp_eq." & Use_PT & " " & Use_R & ", -1");
                              Emit_Inst (Ctx.E, Ov & " = and.i8 " & Im & ", " & M1);
                              Emit_Inst (Ctx.E, Bad & " = or.i8 " & Z & ", " & Ov);
                              Emit_Check_Fail_If (Ctx, Bad);
                           end;
                        end if;
                        Emit_Inst (Ctx.E, Out_R & " = div_s." & Use_PT & " " & Use_L & ", " & Use_R & " [unsafe]");
                     end if;
                  when 17 => -- MOD
                     if Ctx.Enable_Checks then
                        declare
                           Z : constant String := Next_Reg (Ctx.E, "mz");
                        begin
                           Emit_Inst (Ctx.E, Z & " = cmp_eq." & Use_PT & " " & Use_R & ", 0");
                           Emit_Check_Fail_If (Ctx, Z);
                        end;
                     end if;
                     declare
                        RemR : constant String := Next_Reg (Ctx.E, "mr");
                        Nz   : constant String := Next_Reg (Ctx.E, "mnz");
                        RNeg : constant String := Next_Reg (Ctx.E, "mrn");
                        BNeg : constant String := Next_Reg (Ctx.E, "mbn");
                        Diff : constant String := Next_Reg (Ctx.E, "mdif");
                        Adj  : constant String := Next_Reg (Ctx.E, "madj");
                        Sum  : constant String := Next_Reg (Ctx.E, "msum");
                     begin
                        Emit_Inst (Ctx.E, RemR & " = rem_s." & Use_PT & " " & Use_L & ", " & Use_R & " [wrap]");
                        Emit_Inst (Ctx.E, Nz & " = cmp_ne." & Use_PT & " " & RemR & ", 0");
                        Emit_Inst (Ctx.E, RNeg & " = cmp_lt_s." & Use_PT & " " & RemR & ", 0");
                        Emit_Inst (Ctx.E, BNeg & " = cmp_lt_s." & Use_PT & " " & Use_R & ", 0");
                        Emit_Inst (Ctx.E, Diff & " = xor.i8 " & RNeg & ", " & BNeg);
                        Emit_Inst (Ctx.E, Adj & " = and.i8 " & Nz & ", " & Diff);
                        Emit_Inst (Ctx.E, Sum & " = add." & Use_PT & " " & RemR & ", " & Use_R);
                        Emit_Inst (Ctx.E, Out_R & " = select." & Use_PT & " " & Adj & ", " & Sum & ", " & RemR);
                     end;
                  when 18 => -- REM
                     if Ctx.Enable_Checks then
                        declare
                           Z : constant String := Next_Reg (Ctx.E, "rz");
                        begin
                           Emit_Inst (Ctx.E, Z & " = cmp_eq." & Use_PT & " " & Use_R & ", 0");
                           Emit_Check_Fail_If (Ctx, Z);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, Out_R & " = rem_s." & Use_PT & " " & Use_L & ", " & Use_R & " [wrap]");
                  when 20 => -- **
                     if not Is_Void_Tree (A2_N) and then Has_Static_Val (Safe_SM_Value (A2_N)) then
                        declare
                           EVal : constant Integer_64 := Eval_Int64 (Safe_SM_Value (A2_N));
                        begin
                           if EVal = 0 then
                              if Is_Real then
                                 Emit_Inst (Ctx.E, Out_R & " = const." & Use_PT & " 1.0");
                              else
                                 Emit_Inst (Ctx.E, Out_R & " = const." & Use_PT & " 1");
                              end if;
                              Res.Reg := To_Unbounded_String (Out_R);
                              Res.Pir_Type := Op_PT;
                              return Res;
                           elsif EVal = 1 then
                              Res.Reg := L_Reg;
                              Res.Pir_Type := Op_PT;
                              return Res;
                           elsif EVal = 2 then
                              if Is_Real then
                                 Emit_Inst (Ctx.E, Out_R & " = fmul." & Use_PT & " " & Use_L & ", " & Use_L);
                              elsif Ctx.Enable_Checks then
                                 declare
                                    Ovf : constant String := Next_Reg (Ctx.E, "ov");
                                 begin
                                    Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = mul_ovf_s." & Use_PT & " " & Use_L & ", " & Use_L);
                                    Emit_Check_Fail_If (Ctx, Ovf);
                                 end;
                              else
                                 Emit_Inst (Ctx.E, Out_R & " = mul." & Use_PT & " " & Use_L & ", " & Use_L & " [unsafe_s]");
                              end if;
                              Res.Reg := To_Unbounded_String (Out_R);
                              Res.Pir_Type := Op_PT;
                              return Res;
                           end if;
                        end;
                     end if;
                     declare
                        E32 : constant String := Emit_Int_Convert (Ctx, To_String (R_V.Reg), To_String (R_V.Pir_Type), "i32");
                        Chk : constant String := (if Ctx.Enable_Checks then "1" else "0");
                        RV  : Unbounded_String;
                     begin
                        if Is_Real then
                           if Use_PT = "f32" then
                              Emit_Call_Or_Invoke (Ctx, "@__ada_fpow_f32", Use_L & ", " & E32, "f32", RV);
                           else
                              Emit_Call_Or_Invoke (Ctx, "@__ada_fpow_f64", Use_L & ", " & E32, "f64", RV);
                           end if;
                           Res.Reg := RV;
                           Res.Pir_Type := Op_PT;
                           return Res;
                        else
                           if Use_PT = "i64" then
                              Emit_Call_Or_Invoke (Ctx, "@__ada_ipow_i64", Use_L & ", " & E32 & ", " & Chk, "i64", RV);
                              Res.Reg := RV;
                           else
                              declare
                                 L32 : constant String := Emit_Int_Convert (Ctx, Use_L, Use_PT, "i32");
                              begin
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_ipow_i32", L32 & ", " & E32 & ", " & Chk, "i32", RV);
                                 if Ctx.Enable_Checks and then Use_PT /= "i32" then
                                    Emit_Range_Check (Ctx, To_String (RV), "i32", Subst_Base_Type (Ctx, Res_Type));
                                 end if;
                                 Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", Use_PT));
                              end;
                           end if;
                           Res.Pir_Type := Op_PT;
                           return Res;
                        end if;
                     end;
                  when others =>
                     Emit_Inst (Ctx.E, Out_R & " = const.i32 0");
               end case;

               Res.Reg := To_Unbounded_String (Out_R);
               if Op_Pos in 7 .. 12 then
                  Res.Pir_Type := To_Unbounded_String ("i8");
               else
                  Res.Pir_Type := Op_PT;
               end if;
               return Res;
            end;
         end;
      end;
      end;
   end Emit_Builtin_Op;

end Cg_Expr.Ops;
