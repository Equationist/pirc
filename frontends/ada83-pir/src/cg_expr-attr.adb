-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Fixed;
with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;

package body Cg_Expr.Attr is

   function Emit_Attribute (Ctx : in out Context; Node : TREE; Target_Type : TREE) return Expr_Val is
      Attr_Id : constant TREE := D (AS_USED_NAME_ID, Node);
      AName   : constant String := Sym_Name (Attr_Id);
      Pfx     : constant TREE := D (AS_NAME, Node);
      Arg     : constant TREE := D (AS_EXP, Node);
      ET      : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
      Res     : Expr_Val;

      function Is_Type_Prefix (P : TREE) return Boolean is
      begin
         if Is_Void_Tree (P) then
            return False;
         elsif KIND (P) = DN_ATTRIBUTE and then Sym_Name (D (AS_USED_NAME_ID, P)) = "BASE" then
            return True;
         elsif KIND (P) = DN_USED_NAME_ID and then not Is_Void_Tree (D (SM_DEFN, P))
           and then KIND (D (SM_DEFN, P)) in CLASS_TYPE_NAME
         then
            return True;
         elsif KIND (P) = DN_SELECTED then
            declare
               Df : constant TREE := D (SM_DEFN, D (AS_DESIGNATOR, P));
            begin
               return not Is_Void_Tree (Df) and then KIND (Df) in CLASS_TYPE_NAME;
            end;
         end if;
         return False;
      end Is_Type_Prefix;

      function Eval_Fixed_Ticks_Exact (SV : TREE; Sm : IEEE_Float_64) return Integer_64 is
         FVal : constant IEEE_Float_64 := Eval_Float64 (SV);
         Rat  : constant IEEE_Float_64 := FVal / Sm;
      begin
         if KIND (SV) = DN_REAL_VAL then
            declare
               N64 : constant Integer_64 := Eval_Int64 (D (XD_NUMER, SV));
               D64 : constant Integer_64 := Eval_Int64 (D (XD_DENOM, SV));
            begin
               if D64 > 0
                 and then abs (IEEE_Float_64 (N64) / IEEE_Float_64 (D64) - FVal)
                          <= abs (FVal) * 1.0E-12 + 1.0E-30
               then
                  declare
                     DS : constant IEEE_Float_64 := IEEE_Float_64 (D64) * Sm;
                  begin
                     if DS = 1.0 then
                        return N64;
                     elsif DS > 1.0 and then DS < 1.0E15
                       and then IEEE_Float_64 (Integer_64 (DS)) = DS
                       and then N64 mod Integer_64 (DS) = 0
                     then
                        return N64 / Integer_64 (DS);
                     elsif DS > 0.0 and then 1.0 / DS < 1.0E15
                       and then IEEE_Float_64 (Integer_64 (1.0 / DS)) = 1.0 / DS
                     then
                        declare
                           K : constant Integer_64 := Integer_64 (1.0 / DS);
                        begin
                           if K > 0 and then N64 /= Integer_64'First
                             and then abs (N64) <= Integer_64'Last / K
                           then
                              return N64 * K;
                           end if;
                        end;
                     end if;
                  end;
               end if;
            end;
         end if;
         if Rat >= IEEE_Float_64 (Integer_64'Last) then return Integer_64'Last;
         elsif Rat <= IEEE_Float_64 (Integer_64'First) then return Integer_64'First;
         else return Integer_64 (IEEE_Float_64'Rounding (Rat));
         end if;
      end Eval_Fixed_Ticks_Exact;

      function Emit_Fixed_Bound_I64 (Expr_Node : TREE; Base_T : TREE) return String is
         SV : constant TREE := Safe_SM_Value (Expr_Node);
         Sm : constant IEEE_Float_64 := Fixed_Small (Base_T);
      begin
         if Has_Static_Val (SV) and then Sm > 0.0 then
            declare
               Sc  : Integer_64;
               Ovf : Boolean;
               R   : constant String := Next_Reg (Ctx.E, "fxb");
            begin
               Eval_Fixed_Ticks_Exact_Checked (SV, Base_T, Sc, Ovf);
               pragma Unreferenced (Ovf);
               Emit_Inst (Ctx.E, R & " = const.i64 " & Img (Long_Long_Integer (Sc)));
               return R;
            end;
         end if;
         declare
            EV : constant Expr_Val := Emit_Expr (Ctx, Expr_Node, Base_T);
            PT : constant String := To_String (EV.Pir_Type);
         begin
            if PT = "f32" or else PT = "f64" then
               return Emit_F64_To_Fixed_Ticks (Ctx, To_String (EV.Reg), PT, Base_T, "i64");
            else
               return Emit_Int_Convert (Ctx, To_String (EV.Reg), PT, "i64");
            end if;
         end;
      end Emit_Fixed_Bound_I64;

      function Resolve_Prefix_Type (P : TREE) return TREE is
      begin
         if Is_Void_Tree (P) then
            return CONST_VOID;
         elsif KIND (P) = DN_ATTRIBUTE then
            if Sym_Name (D (AS_USED_NAME_ID, P)) = "BASE" then
               return Subst_Base_Type (Ctx, Resolve_Prefix_Type (D (AS_NAME, P)));
            end if;
            return Subst_Tree (Ctx, D (SM_EXP_TYPE, P));
         elsif KIND (P) = DN_SELECTED then
            declare
               Df : constant TREE := D (SM_DEFN, D (AS_DESIGNATOR, P));
            begin
               if not Is_Void_Tree (Df) and then KIND (Df) in CLASS_TYPE_NAME then
                  return Subst_Tree (Ctx, D (SM_TYPE_SPEC, Df));
               elsif not Is_Void_Tree (Df) and then KIND (Df) in CLASS_OBJECT_NAME then
                  return Subst_Tree (Ctx, D (SM_OBJ_TYPE, Df));
               end if;
               return Subst_Tree (Ctx, D (SM_EXP_TYPE, P));
            end;
         elsif KIND (P) = DN_USED_NAME_ID and then not Is_Void_Tree (D (SM_DEFN, P))
           and then KIND (D (SM_DEFN, P)) in CLASS_TYPE_NAME
         then
            return Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (SM_DEFN, P)));
         elsif KIND (P) in CLASS_EXP and then KIND (P) /= DN_USED_NAME_ID and then KIND (P) /= DN_USED_OP then
            return Subst_Tree (Ctx, D (SM_EXP_TYPE, P));
         else
            declare
               Df : constant TREE := Get_Name_Defn (P);
            begin
               if not Is_Void_Tree (Df) then
                  if KIND (Df) in CLASS_TYPE_NAME then
                     return Subst_Tree (Ctx, D (SM_TYPE_SPEC, Df));
                  elsif KIND (Df) in CLASS_OBJECT_NAME then
                     return Subst_Tree (Ctx, D (SM_OBJ_TYPE, Df));
                  end if;
               end if;
            end;
            return CONST_VOID;
         end if;
      end Resolve_Prefix_Type;
   begin
      if Is_Void_Tree (ET) or else KIND (ET) = DN_UNIVERSAL_INTEGER or else KIND (ET) = DN_UNIVERSAL_REAL
        or else KIND (ET) = DN_ANY_INTEGER or else KIND (ET) = DN_ANY_REAL
      then
         if not Is_Void_Tree (Target_Type) and then Is_Scalar_Type (Target_Type) then
            ET := Target_Type;
         end if;
      end if;
      declare
         TI : constant Type_Info := Get_Type_Info (Ctx, ET);
         PT_Str : constant String := (if To_String (TI.Pir_Type) = "void" then "i32" else To_String (TI.Pir_Type));
         Pfx_TS_Check : constant TREE := Resolve_Type_Spec (Resolve_Prefix_Type (Pfx));
         Skip_Static_Fold : constant Boolean :=
           ((AName = "LARGE" or else AName = "SAFE_LARGE" or else AName = "SMALL"
             or else AName = "SAFE_SMALL" or else AName = "EPSILON")
            and then not Is_Void_Tree (Pfx_TS_Check) and then KIND (Pfx_TS_Check) = DN_FLOAT)
           or else (Is_Fixed_Type (ET)
                    and then (AName = "LARGE" or else AName = "SAFE_LARGE")
                    and then Is_Fixed_Type (Pfx_TS_Check))
           or else AName = "MACHINE_OVERFLOWS"
           or else (not Is_Type_Prefix (Pfx)
                    and then (AName = "FIRST" or else AName = "LAST" or else AName = "LENGTH"
                              or else AName = "SIZE" or else AName = "ADDRESS"));
      begin
         -- Static value already folded by semphase!
         -- Skip direct f64 decimal decoding for float binary attrs (LARGE/SMALL/SAFE_*/EPSILON)
         -- and fixed-point LARGE/SAFE_LARGE so exact binary/64-bit math is used below.
         if Has_Static_Val (Safe_SM_Value (Node)) and then not Skip_Static_Fold then
            declare
               SV : constant TREE := Safe_SM_Value (Node);
               R  : constant String := Next_Reg (Ctx.E, "ac");
            begin
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := To_Unbounded_String (PT_Str);
               Res.Align    := TI.Align;
               if Is_Float_Type (ET) then
                  Res.Reg := To_Unbounded_String (Emit_Float_Const_Safe (Ctx, Eval_Float64 (SV), PT_Str, "ac"));
                  return Res;
               elsif Is_Fixed_Type (ET) then
                  declare
                     Sc : constant Integer_64 := Eval_Fixed_Ticks_Exact (SV, Fixed_Small (ET));
                  begin
                     Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Long_Long_Integer (Sc)));
                  end;
               else
                  declare
                     V_I : Integer_64;
                     Ovf : Boolean;
                     Is_U : constant Boolean := Is_Unsigned_Scalar (ET, PT_Str);
                     MinB : constant Integer_64 := (if Is_U then 0 else Integer_64 (Min_Signed_For_Pir (PT_Str)));
                     MaxB : constant Integer_64 :=
                       (if Is_U and then PT_Str = "i8" then 255
                        elsif Is_U and then PT_Str = "i16" then 65535
                        elsif Is_U and then PT_Str = "i32" then 4294967295
                        elsif PT_Str = "i8" then 127 elsif PT_Str = "i16" then 32767
                        elsif PT_Str = "i32" then 2147483647 else Integer_64'Last);
                  begin
                     Eval_Int64_Checked (SV, V_I, Ovf);
                     if Ovf or else V_I < MinB or else V_I > MaxB then
                        if Ctx.Enable_Checks then
                           declare
                              Bd : constant String := Next_Reg (Ctx.E, "aov");
                           begin
                              Emit_Inst (Ctx.E, Bd & " = const.i8 1");
                              Emit_Check_Fail_If (Ctx, Bd);
                           end;
                        end if;
                        Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0");
                     else
                        Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Format_Int_Imm_For_Pir (V_I, PT_Str));
                     end if;
                  end;
               end if;
               Res.Reg := To_Unbounded_String (R);
               return Res;
            end;
         end if;

         -- Array / Scalar attributes: FIRST, LAST, LENGTH
         if AName = "FIRST" or else AName = "LAST" or else AName = "LENGTH" then
            declare
               Dim : Natural := 1;
            begin
               if not Is_Void_Tree (Arg) and then Has_Static_Val (Safe_SM_Value (Arg)) then
                  Dim := Natural (Eval_Int64 (Safe_SM_Value (Arg)));
               end if;
               -- Check if prefix is a type mark (including T'BASE and Pkg.T!)
               if Is_Type_Prefix (Pfx) then
                  declare
                     TS : constant TREE := Resolve_Prefix_Type (Pfx);
                     St : Boolean;
                     Lo, Hi : Integer_64;
                  begin
                     if Is_Discrete_Type (TS) then
                        Get_Discrete_Bounds (TS, St, Lo, Hi);
                        if St then
                           declare
                              Val : constant Integer_64 := (if AName = "FIRST" then Lo else Hi);
                              R   : constant String := Next_Reg (Ctx.E, "tb");
                           begin
                              Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Long_Long_Integer (Val)));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                              return Res;
                           end;
                        else
                           -- Dynamic subtype bounds (check Saved_Type_Ranges first for c36204c, then DN_RANGE / DN_RANGE_ATTRIBUTE!)
                           declare
                              RTS : constant TREE := Subst_Resolve_Type (Ctx, TS);
                              Rng : constant TREE := D (SM_RANGE, RTS);
                           begin
                              declare
                                 SR : Saved_Range_Info;
                              begin
                                 if Try_Lookup_Saved_Range (Ctx, RTS, SR) then
                                    declare
                                       SlotP : constant String := (if AName = "FIRST" then To_String (SR.Lo_Ptr) else To_String (SR.Hi_Ptr));
                                       RV0   : constant String := Load_Saved_Slot (Ctx, SlotP, To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                                    begin
                                       Res.Kind := EK_Scalar;
                                       Res.Reg  := To_Unbounded_String
                                         (Emit_Scalar_Coerce (Ctx, RV0, To_String (SR.Pir_Type), PT_Str,
                                            Is_Unsigned => Is_Unsigned_Scalar (TS, To_String (SR.Pir_Type))));
                                       Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                       return Res;
                                    end;
                                 end if;
                              end;
                              if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                                 declare
                                    E_Bound : constant TREE := (if AName = "FIRST" then D (AS_EXP1, Rng) else D (AS_EXP2, Rng));
                                    BV : constant Expr_Val := Emit_Expr (Ctx, E_Bound, ET);
                                 begin
                                    Res.Kind := EK_Scalar;
                                    Res.Reg := To_Unbounded_String
                                      (Emit_Scalar_Coerce (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), PT_Str,
                                         Is_Unsigned => Is_Unsigned_Scalar (BV.Ada_Type, To_String (BV.Pir_Type))));
                                    Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                    return Res;
                                 end;
                              elsif not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE_ATTRIBUTE then
                                 declare
                                    RT_U, Lo_U, Hi_U : Unbounded_String;
                                 begin
                                    Emit_Discrete_Range (Ctx, Rng, RT_U, Lo_U, Hi_U);
                                    declare
                                       Pick : constant String := (if AName = "FIRST" then To_String (Lo_U) else To_String (Hi_U));
                                    begin
                                       Res.Kind := EK_Scalar;
                                       Res.Reg  := To_Unbounded_String
                                         (Emit_Scalar_Coerce (Ctx, Pick, To_String (RT_U), PT_Str,
                                            Is_Unsigned => Is_Unsigned_Scalar (TS, To_String (RT_U))));
                                       Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                       return Res;
                                    end;
                                 end;
                              else
                                 declare
                                    R : constant String := Next_Reg (Ctx.E, "dbz");
                                 begin
                                    Emit_Inst (Ctx.E, R & " = const." & PT_Str & (if PT_Str = "f32" or else PT_Str = "f64" then " 0.0" else " 0"));
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                    Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                    return Res;
                                 end;
                              end if;
                           end;
                        end if;
                     elsif Is_Float_Type (Subst_Resolve_Type (Ctx, TS)) or else Is_Fixed_Type (Subst_Resolve_Type (Ctx, TS)) then
                        declare
                           RTS : constant TREE := Subst_Resolve_Type (Ctx, TS);
                           Rng : constant TREE := D (SM_RANGE, RTS);
                           R   : constant String := Next_Reg (Ctx.E, "rfb");
                        begin
                           -- P3-2.11: load saved dynamic real subtype bounds without re-evaluating side effects!
                           declare
                              SR : Saved_Range_Info;
                           begin
                              if Try_Lookup_Saved_Range (Ctx, RTS, SR) then
                                 declare
                                    Ptr_S : constant String := To_String (if AName = "FIRST" then SR.Lo_Ptr else SR.Hi_Ptr);
                                    SV_R  : constant String := Load_Saved_Slot (Ctx, Ptr_S, To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                                 begin
                                    Res.Kind := EK_Scalar;
                                    Res.Reg  := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, SV_R, To_String (SR.Pir_Type), PT_Str));
                                    Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                    return Res;
                                 end;
                              end if;
                           end;
                           if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                              declare
                                 E_B : constant TREE := (if AName = "FIRST" then D (AS_EXP1, Rng) else D (AS_EXP2, Rng));
                              begin
                                 if Is_Fixed_Type (RTS) then
                                    declare
                                       B64 : constant String := Emit_Fixed_Bound_I64 (E_B, Subst_Base_Type (Ctx, RTS));
                                    begin
                                       Res.Kind := EK_Scalar;
                                       Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, B64, "i64", PT_Str));
                                       Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                       return Res;
                                    end;
                                 else
                                    if Has_Static_Val (Safe_SM_Value (E_B)) then
                                       Res.Kind := EK_Scalar;
                                       Res.Reg := To_Unbounded_String
                                         (Emit_Float_Const_Safe (Ctx, Eval_Float64 (Safe_SM_Value (E_B)), PT_Str, "rfb"));
                                       Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                       return Res;
                                    end if;
                                    declare
                                       BV : constant Expr_Val := Emit_Expr (Ctx, E_B, ET);
                                    begin
                                       Res.Kind := EK_Scalar;
                                       Res.Reg := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), PT_Str));
                                       Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                       return Res;
                                    end;
                                 end if;
                              end;
                           end if;
                           Emit_Inst (Ctx.E, R & " = const." & PT_Str & (if PT_Str = "f32" or else PT_Str = "f64" then " 0.0" else " 0"));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     elsif Is_Constrained_Array (TS) then
                        declare
                           ATI : constant Type_Info := Get_Type_Info (Ctx, TS);
                        begin
                           if Dim <= ATI.NDims and then ATI.Dims (Dim).Is_Static then
                              declare
                                 LoV  : constant Integer_64 := ATI.Dims (Dim).First;
                                 HiV  : constant Integer_64 := ATI.Dims (Dim).Last;
                                 Val  : Integer_64 := 0;
                                 Ovf  : Boolean := False;
                                 Is_U : constant Boolean := Is_Unsigned_Scalar (ET, PT_Str);
                                 MinB : constant Integer_64 := (if Is_U then 0 else Integer_64 (Min_Signed_For_Pir (PT_Str)));
                                 MaxB : constant Integer_64 :=
                                   (if Is_U and then PT_Str = "i8" then 255
                                    elsif Is_U and then PT_Str = "i16" then 65535
                                    elsif Is_U and then PT_Str = "i32" then 4294967295
                                    elsif PT_Str = "i8" then 127 elsif PT_Str = "i16" then 32767
                                    elsif PT_Str = "i32" then 2147483647 else Integer_64'Last);
                                 R    : constant String := Next_Reg (Ctx.E, "ab");
                              begin
                                 if AName = "FIRST" then
                                    Val := LoV;
                                 elsif AName = "LAST" then
                                    Val := HiV;
                                 elsif HiV >= LoV then
                                    declare
                                       Diff : constant Unsigned_64 := Unsigned_64'Mod (HiV) - Unsigned_64'Mod (LoV);
                                    begin
                                       if Diff >= Unsigned_64 (Integer_64'Last) then
                                          Ovf := True;
                                       else
                                          Val := Integer_64 (Diff + 1);
                                       end if;
                                    end;
                                 else
                                    Val := 0;
                                 end if;
                                 if Ovf or else Val < MinB or else Val > MaxB then
                                    if Ctx.Enable_Checks then
                                       declare
                                          Bd : constant String := Next_Reg (Ctx.E, "abov");
                                       begin
                                          Emit_Inst (Ctx.E, Bd & " = const.i8 1");
                                          Emit_Check_Fail_If (Ctx, Bd);
                                       end;
                                    end if;
                                    Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0");
                                 else
                                    Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Format_Int_Imm_For_Pir (Val, PT_Str));
                                 end if;
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                 return Res;
                              end;
                           elsif Dim <= ATI.NDims then
                              declare
                                 F_R, L_R : Unbounded_String;
                              begin
                                 Emit_Index_Bounds (Ctx, ATI.Dims (Dim).Idx_Type, F_R, L_R);
                                 Res.Kind := EK_Scalar; Res.Ada_Type := ET;
                                 Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Align := TI.Align;
                                 if AName = "FIRST" then
                                    Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F_R), "i64", PT_Str));
                                 elsif AName = "LAST" then
                                    Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L_R), "i64", PT_Str));
                                 else
                                    declare
                                       L64 : constant String := Emit_Dim_Len_Checked_I64 (Ctx, To_String (F_R), To_String (L_R));
                                    begin
                                       if Ctx.Enable_Checks and then Int_Bits_Of_Pir (PT_Str) < 64 then
                                          Emit_Range_Check (Ctx, L64, "i64", ET);
                                          declare
                                             Is_U : constant Boolean := Is_Unsigned_Scalar (ET, PT_Str);
                                             MaxB : constant Long_Long_Integer :=
                                               (if Is_U and then PT_Str = "i8" then 255
                                                elsif Is_U and then PT_Str = "i16" then 65535
                                                elsif Is_U and then PT_Str = "i32" then 4294967295
                                                elsif PT_Str = "i8" then 127 elsif PT_Str = "i16" then 32767
                                                else 2147483647);
                                             Gt : constant String := Next_Reg (Ctx.E, "lgt");
                                          begin
                                             Emit_Inst (Ctx.E, Gt & " = cmp_gt_s.i64 " & L64 & ", " & Img (MaxB));
                                             Emit_Check_Fail_If (Ctx, Gt);
                                          end;
                                       end if;
                                       Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, L64, "i64", PT_Str));
                                    end;
                                 end if;
                                 return Res;
                              end;
                           end if;
                        end;
                     end if;
                  end;
               end if;

               -- Array object prefix (including access-to-array dereference!)
               declare
                  AV : Expr_Val := Emit_LValue (Ctx, Pfx);
                  F_R, L_R : Unbounded_String;
               begin
                  if Is_Access_Type (AV.Ada_Type) then
                     declare
                        Acc_T : constant TREE := Resolve_Type_Spec (AV.Ada_Type);
                        Des_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Acc_T)));
                        Ptr_R : Unbounded_String := AV.Reg;
                     begin
                        if AV.Kind = EK_Addr then
                           declare
                              LP : constant String := Next_Reg (Ctx.E, "afp");
                           begin
                              Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Ptr_R) & ", align 8");
                              Ptr_R := To_Unbounded_String (LP);
                           end;
                        end if;
                        Emit_Null_Check (Ctx, To_String (Ptr_R));
                        if Is_Access_To_Uncon_Array (Acc_T) then
                           Emit_Unpack_Arr_Desc (Ctx, To_String (Ptr_R), Des_T, AV);
                        else
                           AV.Ada_Type := Des_T;
                        end if;
                     end;
                  end if;
                  Get_Array_Dim_Bounds (Ctx, AV, Dim, F_R, L_R);
                  Res.Kind     := EK_Scalar;
                  Res.Ada_Type := ET;
                  Res.Pir_Type := To_Unbounded_String (PT_Str);
                  Res.Align    := TI.Align;
                  if AName = "FIRST" then
                     Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F_R), "i64", PT_Str));
                  elsif AName = "LAST" then
                     Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L_R), "i64", PT_Str));
                  else
                     declare
                        L64 : constant String := Emit_Dim_Len_Checked_I64 (Ctx, To_String (F_R), To_String (L_R));
                     begin
                        if Ctx.Enable_Checks and then Int_Bits_Of_Pir (PT_Str) < 64 then
                           Emit_Range_Check (Ctx, L64, "i64", ET);
                           declare
                              Is_U : constant Boolean := Is_Unsigned_Scalar (ET, PT_Str);
                              MaxB : constant Long_Long_Integer :=
                                (if Is_U and then PT_Str = "i8" then 255
                                 elsif Is_U and then PT_Str = "i16" then 65535
                                 elsif Is_U and then PT_Str = "i32" then 4294967295
                                 elsif PT_Str = "i8" then 127 elsif PT_Str = "i16" then 32767
                                 else 2147483647);
                              Gt : constant String := Next_Reg (Ctx.E, "lgt");
                           begin
                              Emit_Inst (Ctx.E, Gt & " = cmp_gt_s.i64 " & L64 & ", " & Img (MaxB));
                              Emit_Check_Fail_If (Ctx, Gt);
                           end;
                        end if;
                        Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, L64, "i64", PT_Str));
                     end;
                  end if;
                  return Res;
               end;
            end;

         elsif AName = "POS" or else AName = "VAL" or else AName = "SUCC" or else AName = "PRED" then
            return Emit_Discrete_Attr_Op (Ctx, AName, Resolve_Prefix_Type (Pfx), ET, Arg);

         elsif AName = "SIZE" then
            declare
               TS   : constant TREE := Subst_Resolve_Type (Ctx, Resolve_Prefix_Type (Pfx));
               BT   : constant TREE := Subst_Base_Type (Ctx, TS);
               PTI  : constant Type_Info := Get_Type_Info (Ctx, TS);
               Bits : Natural := PTI.Byte_Size * 8;
               R    : constant String := Next_Reg (Ctx.E, "sz");
               Is_Type_Pfx : Boolean := False;
               Explicit_Sz : Natural := 0;
               Obj_Pfx     : Expr_Val;

               function Extract_Size_Attr (T : TREE) return Natural is
                  V : TREE;
               begin
                  if Is_Void_Tree (T) then return 0; end if;
                  if KIND (T) in CLASS_SCALAR then
                     V := D (CD_IMPL_SIZE, T);
                     if Has_Static_Val (V) then
                        declare
                           IV : constant Integer_64 := Eval_Int64 (V);
                        begin
                           if IV > 0 and then IV < 1_000_000 then
                              return Natural (IV);
                           end if;
                        end;
                     end if;
                  elsif KIND (T) = DN_TASK_SPEC
                    or else KIND (T) in CLASS_UNCONSTRAINED
                  then
                     V := D (SM_SIZE, T);
                     if Has_Static_Val (V) then
                        declare
                           IV : constant Integer_64 := Eval_Int64 (V);
                        begin
                           if IV > 0 and then IV < 1_000_000 then
                              return Natural (IV);
                           end if;
                        end;
                     end if;
                  elsif KIND (T) in DN_CONSTRAINED_ARRAY | DN_CONSTRAINED_RECORD then
                     declare
                        BT : constant TREE := D (SM_BASE_TYPE, T);
                     begin
                        if not Is_Void_Tree (BT) and then KIND (BT) in CLASS_UNCONSTRAINED then
                           V := D (SM_SIZE, BT);
                           if Has_Static_Val (V) then
                              declare
                                 IV : constant Integer_64 := Eval_Int64 (V);
                              begin
                                 if IV > 0 and then IV < 1_000_000 then
                                    return Natural (IV);
                                 end if;
                              end;
                           end if;
                        end if;
                     end;
                  end if;
                  return 0;
               end Extract_Size_Attr;

               function Min_Discrete_Bits (Lo, Hi : Integer_64) return Natural is
                  B : Natural := 1;
               begin
                  if Hi < Lo then
                     return 1;
                  elsif Lo >= 0 then
                     while B < 63 and then (Shift_Left (Unsigned_64 (1), B) - 1) < Unsigned_64 (Hi) loop
                        B := B + 1;
                     end loop;
                     return B;
                  else
                     -- Signed two's complement: -(2^(B-1)) .. 2^(B-1)-1
                     while B < 64 loop
                        declare
                           Pos_Max : constant Integer_64 := Integer_64 (Shift_Left (Unsigned_64 (1), B - 1) - 1);
                           Neg_Min : constant Integer_64 := -Pos_Max - 1;
                        begin
                           exit when Lo >= Neg_Min and then Hi <= Pos_Max;
                        end;
                        B := B + 1;
                     end loop;
                     return B;
                  end if;
               end Min_Discrete_Bits;
            begin
               declare
                  PD : constant TREE := Get_Name_Defn (Pfx);
               begin
                  if not Is_Void_Tree (PD) and then KIND (PD) in CLASS_TYPE_NAME then
                     Is_Type_Pfx := True;
                  elsif Is_Type_Prefix (Pfx) then
                     Is_Type_Pfx := True;
                  end if;
               Explicit_Sz := Extract_Size_Attr (TS);
               if Explicit_Sz = 0 then
                  Explicit_Sz := Extract_Size_Attr (BT);
               end if;

               if Is_Type_Pfx then
                  if Is_Discrete_Type (TS) then
                     declare
                        St, BSt : Boolean;
                        Lo, Hi, BLo, BHi : Integer_64;
                        TS_Sz : constant Natural := Extract_Size_Attr (TS);
                        BT_Sz : constant Natural := Extract_Size_Attr (BT);
                     begin
                        Get_Discrete_Bounds (TS, St, Lo, Hi);
                        Get_Discrete_Bounds (BT, BSt, BLo, BHi);
                        declare
                           First_Named : constant TREE :=
                             (if not Is_Void_Tree (BT) and then not Is_Void_Tree (D (XD_SOURCE_NAME, BT))
                                and then KIND (D (XD_SOURCE_NAME, BT)) in CLASS_TYPE_NAME
                              then Resolve_Type_Spec (D (SM_TYPE_SPEC, D (XD_SOURCE_NAME, BT)))
                              else CONST_VOID);
                           FN_St : Boolean := False;
                           FN_Lo, FN_Hi : Integer_64 := 0;
                           Is_Narrower_Sub : Boolean := False;
                        begin
                           if not Is_Void_Tree (First_Named) then
                              Get_Discrete_Bounds (First_Named, FN_St, FN_Lo, FN_Hi);
                           end if;
                           if St and then not Is_Void_Tree (PD) and then KIND (PD) = DN_SUBTYPE_ID then
                              if FN_St then
                                 Is_Narrower_Sub := (Lo > FN_Lo or else Hi < FN_Hi);
                              elsif BSt then
                                 Is_Narrower_Sub := (Lo > BLo or else Hi < BHi);
                              end if;
                           end if;
                           if Is_Narrower_Sub and then (TS_Sz = 0 or else TS_Sz = BT_Sz) then
                              Bits := Min_Discrete_Bits (Lo, Hi);
                           elsif TS_Sz > 0 then
                              Bits := TS_Sz;
                           elsif BT_Sz > 0 then
                              Bits := BT_Sz;
                           elsif St then
                              Bits := Min_Discrete_Bits (Lo, Hi);
                           end if;
                        end;
                     end;
                  elsif Explicit_Sz > 0 then
                     Bits := Explicit_Sz;
                  elsif PTI.Kind = TK_Unconstrained_Array
                    or else (PTI.Kind = TK_Constrained_Array and then PTI.Is_Dynamic)
                  then
                     Bits := 16_777_216;
                  end if;
               else
                  -- P3-3.20: Evaluate object prefix ONCE (reuse for dynamic/unconstrained arrays!)
                  Obj_Pfx := Emit_LValue (Ctx, Pfx);
                  -- Object'SIZE must be >= Type'SIZE and >= storage byte size * 8;
                  -- for dynamic or unconstrained array objects, compute length * elem_bits at runtime!
                  if PTI.Kind = TK_Unconstrained_Array
                    or else (PTI.Kind = TK_Constrained_Array and then PTI.Is_Dynamic)
                  then
                     declare
                        AV     : constant Expr_Val := Obj_Pfx;
                        ETI    : constant Type_Info := Get_Type_Info (Ctx, PTI.Elem_Type);
                        ESz    : constant Natural := Elem_Storage_Size (ETI);
                        ND     : constant Natural := Natural'Max (1, Natural'Min (PTI.NDims, Max_Array_Dims));
                        Fs, Ls : Str_Array_4;
                        Tot64, Byt64 : Unbounded_String;
                        Bits64 : constant String := Next_Reg (Ctx.E, "dszb");
                        OvfB   : constant String := Next_Reg (Ctx.E, "dszo");
                        Conv_R : Unbounded_String;
                     begin
                        for D_I in 1 .. ND loop
                           Get_Array_Dim_Bounds (Ctx, AV, D_I, Fs (D_I), Ls (D_I));
                        end loop;
                        Emit_Array_Byte_Size_I64 (Ctx, Fs, Ls, ND, ESz, Tot64, Byt64);
                        if Ctx.Enable_Checks then
                           Emit_Inst (Ctx.E, Bits64 & ", " & OvfB & " = mul_ovf_s.i64 " & To_String (Byt64) & ", 8");
                           Emit_Check_Fail_If (Ctx, OvfB);
                           if Int_Bits_Of_Pir (PT_Str) < 64 then
                              Emit_Range_Check (Ctx, Bits64, "i64", ET);
                              declare
                                 Is_U : constant Boolean := Is_Unsigned_Scalar (ET, PT_Str);
                                 MaxB : constant Long_Long_Integer :=
                                   (if Is_U and then PT_Str = "i8" then 255
                                    elsif Is_U and then PT_Str = "i16" then 65535
                                    elsif Is_U and then PT_Str = "i32" then 4294967295
                                    elsif PT_Str = "i8" then 127 elsif PT_Str = "i16" then 32767
                                    else 2147483647);
                                 Gt : constant String := Next_Reg (Ctx.E, "sgt");
                              begin
                                 Emit_Inst (Ctx.E, Gt & " = cmp_gt_s.i64 " & Bits64 & ", " & Img (MaxB));
                                 Emit_Check_Fail_If (Ctx, Gt);
                              end;
                           end if;
                        else
                           Emit_Inst (Ctx.E, Bits64 & " = mul.i64 " & To_String (Byt64) & ", 8");
                        end if;
                        Conv_R := To_Unbounded_String (Emit_Int_Convert (Ctx, Bits64, "i64", PT_Str));
                        Res.Kind := EK_Scalar; Res.Reg := Conv_R;
                        Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  end if;
               if Explicit_Sz > Bits then
                  Bits := Explicit_Sz;
               end if;
               -- LRM 13.7.2: Obj.Bitfield'Size yields the component's bitfield width when a record rep clause applies
               if KIND (Pfx) = DN_SELECTED then
                  declare
                     Des_S : constant TREE := D (AS_DESIGNATOR, Pfx);
                     Def_S : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des_S)));
                  begin
                     if not Is_Void_Tree (Def_S) and then KIND (Def_S) in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                        declare
                           PR_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, D (AS_NAME, Pfx))));
                           Rec_T : constant TREE :=
                             (if Is_Access_Type (PR_T) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, PR_T))) else PR_T);
                           Dummy_RT : constant Type_Info := Get_Type_Info (Ctx, Rec_T);
                           pragma Unreferenced (Dummy_RT);
                        begin
                           -- P3-6.5: Honor explicit component clause bit width even for wide components (>= natural size)
                           if Ctx.Fields.Contains (Tree_Key (Def_S))
                             and then Ctx.Fields.Element (Tree_Key (Def_S)).Has_Explicit_Offset
                             and then Ctx.Fields.Element (Tree_Key (Def_S)).Bit_Size > 0
                           then
                              Bits := Ctx.Fields.Element (Tree_Key (Def_S)).Bit_Size;
                           end if;
                        end;
                     end if;
                  end;
               end if;
               end if;
               end;
               if Bits = 0 then Bits := 8; end if;
               Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Bits));
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "ADDRESS" then
            declare
               PDef : TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Pfx)));
            begin
               -- P3-3.16: Unwrap renamed/derived subprogram chains before taking Subprogram'Address!
               for UStep in 1 .. 16 loop
                  exit when Is_Void_Tree (PDef) or else KIND (PDef) not in CLASS_SUBPROG_NAME;
                  declare
                     UD : TREE := D (SM_UNIT_DESC, PDef);
                  begin
                     if Is_Void_Tree (UD) and then Ctx.Subp_Renames.Contains (Tree_Key (PDef)) then
                        UD := Ctx.Subp_Renames.Element (Tree_Key (PDef));
                     end if;
                     exit when Is_Void_Tree (UD);
                     if KIND (UD) = DN_DERIVED_SUBPROG then
                        PDef := Canonical_Def (Subst_Tree (Ctx, D (SM_DERIVABLE, UD)));
                     elsif KIND (UD) = DN_RENAMES_UNIT then
                        declare
                           RN : constant TREE := D (AS_NAME, UD);
                        begin
                           exit when Is_Void_Tree (RN) or else KIND (RN) = DN_ATTRIBUTE;
                           PDef := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (RN)));
                        end;
                     else
                        exit;
                     end if;
                  end;
               end loop;
               if not Is_Void_Tree (PDef) and then KIND (PDef) in CLASS_SUBPROG_NAME then
                  -- LRM 13.7.2: Subprogram'Address
                  declare
                     Key : constant Unsigned_32 := Tree_Key (PDef);
                     SI  : Subp_Info;
                     FA  : constant String := Next_Reg (Ctx.E, "fa");
                     R   : constant String := Next_Reg (Ctx.E, "ad");
                  begin
                     if Ctx.Subps.Contains (Key) then
                        SI := Ctx.Subps.Element (Key);
                        if not Is_Void_Tree (D (SM_INTERFACE, PDef)) then
                           Ensure_Extern_Subp (Ctx, PDef, SI);
                        end if;
                     else
                        declare
                           Spec : constant TREE := D (SM_SPEC, PDef);
                        begin
                           SI.Spec_Node      := Spec;
                           SI.Pir_Name       := To_Unbounded_String (Mangle_Subp_Name (Ctx, PDef, Spec, Is_Subunit => Ctx.In_Subunit));
                           SI.Is_Function    := KIND (Spec) = DN_FUNCTION_SPEC;
                           SI.Is_External    := True;
                           SI.Is_C_Interface := Is_C_Interfaced (PDef);
                           if SI.Is_Function then
                              declare
                                 RDef : constant TREE := Get_Name_Defn (D (AS_NAME, Spec));
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
                           Ctx.Subps.Insert (Key, SI);
                           Ensure_Extern_Subp (Ctx, PDef, SI);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, FA & " = addr " & To_String (SI.Pir_Name));
                     Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & FA);
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := To_Unbounded_String ("i64"); Res.Ada_Type := ET; Res.Align := 8;
                     return Res;
                  end;
               elsif not Is_Void_Tree (PDef) and then KIND (PDef) = DN_PACKAGE_ID then
                  -- P3-6.8 (LRM 13.7.2): Package'Address returns non-zero unique package symbol address
                  declare
                     Sym : constant String := "@ada_pkg_addr_" & Qualified_Ada_Name (PDef) & "_" & Tree_Key_Str (PDef);
                     PA  : constant String := Next_Reg (Ctx.E, "pka");
                     R   : constant String := Next_Reg (Ctx.E, "ad");
                  begin
                     Add_Data_Decl (Ctx.E, Sym, Sym & " = data align 1 { 0 }");
                     Emit_Inst (Ctx.E, PA & " = addr " & Sym);
                     Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & PA);
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := To_Unbounded_String ("i64"); Res.Ada_Type := ET; Res.Align := 8;
                     return Res;
                  end;
               elsif not Is_Void_Tree (PDef) and then KIND (PDef) = DN_LABEL_ID then
                  -- P3-6.8 (LRM 13.7.2): Label'Address returns non-zero unique label symbol address
                  declare
                     Sym : constant String := "@ada_lbl_addr_" & Tree_Key_Str (PDef) & To_String (Ctx.Gen_Inst_Tag);
                     LA  : constant String := Next_Reg (Ctx.E, "lba");
                     R   : constant String := Next_Reg (Ctx.E, "ad");
                  begin
                     Add_Data_Decl (Ctx.E, Sym, Sym & " = data align 1 { 0 }");
                     Emit_Inst (Ctx.E, LA & " = addr " & Sym);
                     Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & LA);
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := To_Unbounded_String ("i64"); Res.Ada_Type := ET; Res.Align := 8;
                     return Res;
                  end;
               elsif not Is_Void_Tree (PDef) and then KIND (PDef) = DN_ENTRY_ID then
                  -- P3-6.8 & P3-6.10 (LRM 13.7.2, 13.5.1): Entry'Address returns interrupt address or unique entry symbol
                  if Ctx.Entries.Contains (Tree_Key (PDef)) and then Ctx.Entries.Element (Tree_Key (PDef)).Has_Address then
                     declare
                        AEV : constant Expr_Val := Emit_Expr (Ctx, Ctx.Entries.Element (Tree_Key (PDef)).Addr_Exp);
                        A64 : constant String := Emit_Int_Convert (Ctx, To_String (AEV.Reg), To_String (AEV.Pir_Type), "i64");
                     begin
                        Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (A64);
                        Res.Pir_Type := To_Unbounded_String ("i64"); Res.Ada_Type := ET; Res.Align := 8;
                        return Res;
                     end;
                  end if;
                  declare
                     Sym : constant String := "@ada_ent_addr_" & Tree_Key_Str (PDef) & To_String (Ctx.Gen_Inst_Tag);
                     EA  : constant String := Next_Reg (Ctx.E, "ena");
                     R   : constant String := Next_Reg (Ctx.E, "ad");
                  begin
                     Add_Data_Decl (Ctx.E, Sym, Sym & " = data align 1 { 0 }");
                     Emit_Inst (Ctx.E, EA & " = addr " & Sym);
                     Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & EA);
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := To_Unbounded_String ("i64"); Res.Ada_Type := ET; Res.Align := 8;
                     return Res;
                  end;
               end if;
            end;
            declare
               LV : constant Expr_Val := Emit_LValue (Ctx, Pfx);
               R  : constant String := Next_Reg (Ctx.E, "ad");
            begin
               if LV.Kind = EK_Scalar or else Length (LV.Reg) = 0 then
                  -- LRM 13.7.2: scalar constants and IN parameters must have a stable address (C'Address = C'Address)
                  declare
                     PDef_A : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Pfx)));
                     Got_Stable : Boolean := False;
                  begin
                     if not Is_Void_Tree (PDef_A) and then Ctx.Vars.Contains (Tree_Key (PDef_A)) then
                        declare
                           VK_A : constant Unsigned_32 := Tree_Key (PDef_A);
                           V_A  : Var_Info := Ctx.Vars.Element (VK_A);
                           ATi  : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
                        begin
                           if V_A.Kind = VK_Const_Int or else V_A.Kind = VK_Const_Float then
                              declare
                                 CSym : constant String := "@ada_caddr_" & Tree_Key_Str (PDef_A) & To_String (Ctx.Gen_Inst_Tag);
                                 CA   : constant String := Next_Reg (Ctx.E, "cad");
                              begin
                                 Add_Global_Decl (Ctx.E, CSym,
                                   CSym & " = global " & To_String (ATi.Pir_Type) & " = zeroinit, align " & Img (Natural'Max (1, ATi.Align)));
                                 Emit_Inst (Ctx.E, CA & " = addr " & CSym);
                                 if Length (LV.Reg) > 0 and then Length (LV.Pir_Type) > 0 then
                                    Emit_Inst (Ctx.E, "store." & To_String (LV.Pir_Type) & " " & To_String (LV.Reg) & ", " & CA & ", align " & Img (Natural'Max (1, ATi.Align)));
                                 end if;
                                 Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & CA);
                                 Got_Stable := True;
                              end;
                           elsif V_A.Kind = VK_Param_Val and then V_A.Owner_Subp = Ctx.Cur_Subp_Key then
                              declare
                                 Slot : Unbounded_String := V_A.Desc_Global;
                              begin
                                 if Length (Slot) = 0 then
                                    Slot := To_Unbounded_String (Next_Reg (Ctx.E, "pads"));
                                    Emit_Entry_Alloca (Ctx.E, To_String (Slot) & " = alloca [" & Img (Natural'Max (1, ATi.Byte_Size)) & " x i8], align " & Img (Natural'Max (1, ATi.Align)));
                                    V_A.Desc_Global := Slot;
                                    Ctx.Vars.Replace (VK_A, V_A);
                                 end if;
                                 if Length (LV.Reg) > 0 and then Length (LV.Pir_Type) > 0 then
                                    Emit_Inst (Ctx.E, "store." & To_String (LV.Pir_Type) & " " & To_String (LV.Reg) & ", " & To_String (Slot) & ", align " & Img (Natural'Max (1, ATi.Align)));
                                 end if;
                                 Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & To_String (Slot));
                                 Got_Stable := True;
                              end;
                           elsif V_A.Kind = VK_Param_Val and then V_A.Owner_Subp /= Ctx.Cur_Subp_Key and then V_A.Captured_Slot > 0 then
                              -- P3-6.8: Uplevel IN parameter has a stable captured slot in parent's static-link frame!
                              declare
                                 UP : constant String := Emit_Captured_Slot_Ptr (Ctx, V_A.Owner_Subp, V_A.Captured_Slot);
                                 LP : constant String := Next_Reg (Ctx.E, "uap");
                              begin
                                 Emit_Inst (Ctx.E, LP & " = load.ptr " & UP & ", align 8");
                                 Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & LP);
                                 Got_Stable := True;
                              end;
                           end if;
                        end;
                     end if;
                     if not Got_Stable then
                        declare
                           ATi  : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
                           Slot : constant String := Next_Reg (Ctx.E, "ads");
                        begin
                           Emit_Entry_Alloca (Ctx.E, Slot & " = alloca [" & Img (Natural'Max (1, ATi.Byte_Size)) & " x i8], align " & Img (Natural'Max (1, ATi.Align)));
                           if Length (LV.Reg) > 0 and then Length (LV.Pir_Type) > 0 then
                              Emit_Inst (Ctx.E, "store." & To_String (LV.Pir_Type) & " " & To_String (LV.Reg) & ", " & Slot & ", align " & Img (Natural'Max (1, ATi.Align)));
                           end if;
                           Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & Slot);
                        end;
                     end if;
                  end;
               else
                  Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & To_String (LV.Reg));
               end if;
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String ("i64"); Res.Ada_Type := ET; Res.Align := 8;
               return Res;
            end;

         elsif AName = "WIDTH" then
            declare
               TS : constant TREE := Subst_Resolve_Type (Ctx, Resolve_Prefix_Type (Pfx));
               BT : constant TREE := Subst_Base_Type (Ctx, TS);
               W  : Natural := 0;
               St : Boolean;
               Lo, Hi : Integer_64;
            begin
               Get_Discrete_Bounds (TS, St, Lo, Hi);
               if not St and then not Is_Void_Tree (TS) then
                  declare
                     Has_Dyn_W : Boolean := False;
                     Lo64, Hi64 : Unbounded_String;
                     SR_W : Saved_Range_Info;
                  begin
                     if Try_Lookup_Saved_Range (Ctx, TS, SR_W) then
                        declare
                           LR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_W.Lo_Ptr), To_String (SR_W.Pir_Type), SR_W.Align, SR_W.Is_Global);
                           HR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_W.Hi_Ptr), To_String (SR_W.Pir_Type), SR_W.Align, SR_W.Is_Global);
                           SU  : constant Boolean := Is_Unsigned_Scalar (TS, To_String (SR_W.Pir_Type));
                        begin
                           Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, LR0, To_String (SR_W.Pir_Type), "i64", Is_Unsigned => SU));
                           Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, HR0, To_String (SR_W.Pir_Type), "i64", Is_Unsigned => SU));
                           Has_Dyn_W := True;
                        end;
                     elsif not Is_Void_Tree (D (SM_RANGE, TS))
                       and then KIND (D (SM_RANGE, TS)) in DN_RANGE | DN_RANGE_ATTRIBUTE
                     then
                        declare
                           RT_W, RL_W, RH_W : Unbounded_String;
                           WU : Boolean;
                        begin
                           Emit_Discrete_Range (Ctx, D (SM_RANGE, TS), RT_W, RL_W, RH_W);
                           WU := Is_Unsigned_Scalar (TS, To_String (RT_W));
                           Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL_W), To_String (RT_W), "i64", Is_Unsigned => WU));
                           Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH_W), To_String (RT_W), "i64", Is_Unsigned => WU));
                           Has_Dyn_W := True;
                        end;
                     end if;
                     if Has_Dyn_W then
                        declare
                           RV : Unbounded_String;
                        begin
                           if not Is_Void_Tree (BT) and then KIND (BT) = DN_ENUMERATION then
                              declare
                                 PS  : Unbounded_String;
                                 Cnt : Natural;
                                 PA  : constant String := Next_Reg (Ctx.E, "ewa");
                              begin
                                 Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                 Emit_Inst (Ctx.E, PA & " = addr " & To_String (PS));
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_enum_width",
                                   PA & ", " & Img (Cnt) & ", " & To_String (Lo64) & ", " & To_String (Hi64), "i32", RV, Nounwind => True);
                              end;
                           else
                              Emit_Call_Or_Invoke (Ctx, "@__ada_int_width", To_String (Lo64) & ", " & To_String (Hi64), "i32", RV, Nounwind => True);
                           end if;
                           Res.Kind := EK_Scalar;
                           Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", PT_Str));
                           Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     end if;
                  end;
               end if;
               if St and then Hi < Lo then
                  W := 0;
               elsif Is_Integer_Type (BT) then
                  if St then
                     declare
                        S1 : constant String := Ada.Strings.Fixed.Trim (Integer_64'Image (Lo), Ada.Strings.Both);
                        S2 : constant String := Ada.Strings.Fixed.Trim (Integer_64'Image (Hi), Ada.Strings.Both);
                        L1 : constant Natural := (if Lo >= 0 then S1'Length + 1 else S1'Length);
                        L2 : constant Natural := (if Hi >= 0 then S2'Length + 1 else S2'Length);
                     begin
                        W := Natural'Max (L1, L2);
                     end;
                  else
                     W := 11;
                  end if;
               elsif not Is_Void_Tree (BT) and then KIND (BT) = DN_ENUMERATION then
                  declare
                     Lits : SEQ_TYPE := LIST (D (SM_LITERAL_S, BT));
                     Lit  : TREE;
                     Idx  : Integer_64 := 0;
                  begin
                     while not IS_EMPTY (Lits) loop
                        POP (Lits, Lit);
                        if not St or else (Idx >= Lo and then Idx <= Hi) then
                           declare
                              Raw : constant String := Sym_Name (Lit);
                              N   : constant Natural :=
                                (if Is_Character_Type (BT) and then Raw'Length >= 2 and then Raw (Raw'First) = '_'
                                 then Raw'Length - 1
                                 else Raw'Length);
                           begin
                              if N > W then W := N; end if;
                           end;
                        end if;
                        Idx := Idx + 1;
                     end loop;
                  end;
               else
                  W := 11;
               end if;
               declare
                  R : constant String := Next_Reg (Ctx.E, "wd");
               begin
                  Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (W));
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                  return Res;
               end;
            end;

         elsif AName = "DIGITS" or else AName = "MANTISSA" or else AName = "EMAX"
           or else AName = "SAFE_EMAX" or else AName = "MACHINE_RADIX"
           or else AName = "MACHINE_MANTISSA" or else AName = "MACHINE_EMAX"
           or else AName = "MACHINE_EMIN"
         then
            declare
               TS   : constant TREE := Resolve_Type_Spec (Resolve_Prefix_Type (Pfx));
               BT   : constant TREE := Base_Type_Spec (TS);
            begin
               -- Fixed-point 'MANTISSA (supports static, dynamic, and generic formal fixed types)
               if AName = "MANTISSA" and then Is_Fixed_Type (TS) then
                  declare
                     Rng  : constant TREE := D (SM_RANGE, TS);
                     BSm  : constant IEEE_Float_64 := Fixed_Small (BT);
                     SSm  : constant IEEE_Float_64 := Fixed_Attr_Small (TS);
                     Lo64 : Unbounded_String;
                     Hi64 : Unbounded_String;
                     BSR  : constant String := Next_Reg (Ctx.E, "fbs");
                     SSR  : constant String := Next_Reg (Ctx.E, "fss");
                     RV   : Unbounded_String;
                     SR_M : Saved_Range_Info;
                  begin
                     if Try_Lookup_Saved_Range (Ctx, TS, SR_M) then
                        declare
                           L0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_M.Lo_Ptr), To_String (SR_M.Pir_Type), SR_M.Align, SR_M.Is_Global);
                           H0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_M.Hi_Ptr), To_String (SR_M.Pir_Type), SR_M.Align, SR_M.Is_Global);
                        begin
                           Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, L0, To_String (SR_M.Pir_Type), "i64"));
                           Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, H0, To_String (SR_M.Pir_Type), "i64"));
                        end;
                     elsif not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                        Lo64 := To_Unbounded_String (Emit_Fixed_Bound_I64 (D (AS_EXP1, Rng), BT));
                        Hi64 := To_Unbounded_String (Emit_Fixed_Bound_I64 (D (AS_EXP2, Rng), BT));
                     else
                        declare
                           Max_B : constant Integer := (if Scalar_Bits (BT) <= 32 then 31 else 63);
                           R     : constant String := Next_Reg (Ctx.E, "fm");
                        begin
                           Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Max_B));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     end if;
                     Emit_Inst (Ctx.E, BSR & " = const.f64 " & Format_Float64 (BSm));
                     Emit_Inst (Ctx.E, SSR & " = const.f64 " & Format_Float64 (SSm));
                     Emit_Call_Or_Invoke (Ctx, "@__ada_fixed_mantissa",
                       To_String (Lo64) & ", " & To_String (Hi64) & ", " & BSR & ", " & SSR, "i32", RV, Nounwind => True);
                     Res.Kind := EK_Scalar;
                     Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", PT_Str));
                     Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  end;
               end if;
               declare
                  Is32 : constant Boolean := Scalar_Bits (TS) <= 32;
                  Digs : Integer := (if Is32 then 6 else 15);
                  BDig : Integer := (if Is32 then 6 else 15);
                  Mant : Integer;
                  BMnt : Integer;
                  Val  : Integer := 0;
                  R    : constant String := Next_Reg (Ctx.E, "fa");
               begin
                  if not Is_Void_Tree (TS) and then KIND (TS) = DN_FLOAT
                    and then Has_Static_Val (D (SM_ACCURACY, TS))
                  then
                     Digs := Integer (Eval_Int64 (D (SM_ACCURACY, TS)));
                  end if;
                  if not Is_Void_Tree (BT) and then KIND (BT) = DN_FLOAT
                    and then Has_Static_Val (D (SM_ACCURACY, BT))
                  then
                     BDig := Integer (Eval_Int64 (D (SM_ACCURACY, BT)));
                  end if;
                  Mant := Integer (IEEE_Float_64'Ceiling (IEEE_Float_64 (Digs) * 3.32192809488736234787)) + 1;
                  BMnt := Integer (IEEE_Float_64'Ceiling (IEEE_Float_64 (BDig) * 3.32192809488736234787)) + 1;
                  if AName = "DIGITS" then
                     Val := Digs;
                  elsif AName = "MANTISSA" then
                     Val := Mant;
                  elsif AName = "EMAX" then
                     Val := 4 * Mant;
                  elsif AName = "SAFE_EMAX" then
                     Val := (if Is32 then 125 else 1021);
                  elsif AName = "MACHINE_RADIX" then
                     Val := 2;
                  elsif AName = "MACHINE_MANTISSA" then
                     Val := (if Is32 then 24 else 53);
                  elsif AName = "MACHINE_EMAX" then
                     Val := (if Is32 then 128 else 1024);
                  elsif AName = "MACHINE_EMIN" then
                     Val := (if Is32 then -125 else -1021);
                  end if;
                  Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Val));
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                  return Res;
               end;
            end;

         elsif AName = "SMALL" or else AName = "LARGE" or else AName = "EPSILON"
           or else AName = "SAFE_SMALL" or else AName = "SAFE_LARGE" or else AName = "DELTA"
         then
            declare
               TS   : constant TREE := Resolve_Type_Spec (Resolve_Prefix_Type (Pfx));
               BT   : constant TREE := Base_Type_Spec (TS);
            begin
               -- Fixed-point real attributes: DELTA, SMALL, SAFE_SMALL, LARGE, SAFE_LARGE
               if Is_Fixed_Type (TS) then
                  if AName = "LARGE" or else AName = "SAFE_LARGE" then
                     declare
                        Use_T : constant TREE := (if AName = "SAFE_LARGE" then BT else TS);
                        Rng   : constant TREE := D (SM_RANGE, Use_T);
                        BSm   : constant IEEE_Float_64 := Fixed_Small (BT);
                        SSm   : constant IEEE_Float_64 := (if AName = "SAFE_LARGE" then BSm else Fixed_Attr_Small (TS));
                        Lo64  : Unbounded_String;
                        Hi64  : Unbounded_String;
                        BSR   : constant String := Next_Reg (Ctx.E, "flbs");
                        SSR   : constant String := Next_Reg (Ctx.E, "flss");
                        Ticks : Unbounded_String;
                        SR_L  : Saved_Range_Info;
                     begin
                        if Try_Lookup_Saved_Range (Ctx, Use_T, SR_L) then
                           declare
                              L0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_L.Lo_Ptr), To_String (SR_L.Pir_Type), SR_L.Align, SR_L.Is_Global);
                              H0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_L.Hi_Ptr), To_String (SR_L.Pir_Type), SR_L.Align, SR_L.Is_Global);
                           begin
                              Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, L0, To_String (SR_L.Pir_Type), "i64"));
                              Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, H0, To_String (SR_L.Pir_Type), "i64"));
                           end;
                        elsif not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                           Lo64 := To_Unbounded_String (Emit_Fixed_Bound_I64 (D (AS_EXP1, Rng), BT));
                           Hi64 := To_Unbounded_String (Emit_Fixed_Bound_I64 (D (AS_EXP2, Rng), BT));
                        else
                           declare
                              Max_T : constant Integer_64 := (if Scalar_Bits (BT) <= 32 then 2147483647 else Integer_64'Last);
                              LR    : constant String := Next_Reg (Ctx.E, "fll");
                           begin
                              Emit_Inst (Ctx.E, LR & " = const.i64 " & Img (Long_Long_Integer (Max_T)));
                              Lo64 := To_Unbounded_String (LR);
                              Hi64 := To_Unbounded_String (LR);
                           end;
                        end if;
                        Emit_Inst (Ctx.E, BSR & " = const.f64 " & Format_Float64 (BSm));
                        Emit_Inst (Ctx.E, SSR & " = const.f64 " & Format_Float64 (SSm));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_fixed_large_ticks",
                          To_String (Lo64) & ", " & To_String (Hi64) & ", " & BSR & ", " & SSR, "i64", Ticks, Nounwind => True);
                        if Is_Fixed_Type (ET) then
                           declare
                              ET_Sm : constant IEEE_Float_64 := Fixed_Small (ET);
                           begin
                              if ET_Sm = BSm then
                                 Res.Kind := EK_Scalar;
                                 Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Ticks), "i64", PT_Str));
                              else
                                 declare
                                    Fl : constant String := Next_Reg (Ctx.E, "flf");
                                    Sc : constant String := Next_Reg (Ctx.E, "fls");
                                 begin
                                    Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (Ticks));
                                    Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (BSm));
                                    Res.Kind := EK_Scalar;
                                    Res.Reg  := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, Sc, "f64", ET, PT_Str));
                                 end;
                              end if;
                           end;
                        elsif PT_Str = "f32" or else PT_Str = "f64" then
                           declare
                              Fl : constant String := Next_Reg (Ctx.E, "flf");
                              Sc : constant String := Next_Reg (Ctx.E, "fls");
                           begin
                              Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (Ticks));
                              Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (BSm));
                              Res.Kind := EK_Scalar;
                              Res.Reg  := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, Sc, "f64", PT_Str));
                           end;
                        else
                           Res.Kind := EK_Scalar;
                           Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Ticks), "i64", PT_Str));
                        end if;
                        Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  else
                     declare
                        FV : IEEE_Float_64;
                        R  : constant String := Next_Reg (Ctx.E, "fxa");
                     begin
                        if AName = "DELTA" then
                           FV := Fixed_Delta (TS);
                        elsif AName = "SAFE_SMALL" then
                           FV := Fixed_Small (BT);
                        else -- SMALL
                           FV := Fixed_Attr_Small (TS);
                        end if;
                        if PT_Str = "f32" or else PT_Str = "f64" then
                           Res.Kind := EK_Scalar;
                           Res.Reg := To_Unbounded_String (Emit_Float_Const_Safe (Ctx, FV, PT_Str, "fxa"));
                           Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        elsif Is_Fixed_Type (ET) then
                           declare
                              FVal : constant IEEE_Float_64 := FV / Fixed_Small (ET);
                              Sc   : Integer_64;
                           begin
                              if FVal >= IEEE_Float_64 (Integer_64'Last) then Sc := Integer_64'Last;
                              elsif FVal <= IEEE_Float_64 (Integer_64'First) then Sc := Integer_64'First;
                              else Sc := Integer_64 (IEEE_Float_64'Rounding (FVal));
                              end if;
                              Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Long_Long_Integer (Sc)));
                           end;
                        else
                           Emit_Inst (Ctx.E, R & " = const." & PT_Str & (if PT_Str = "f32" or else PT_Str = "f64" then " 0.0" else " 0"));
                        end if;
                        Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                        Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  end if;
               end if;

               declare
                  Is32 : constant Boolean := Scalar_Bits (TS) <= 32;
                  Digs : Integer := (if Is32 then 6 else 15);
                  BDig : Integer := (if Is32 then 6 else 15);
                  Mant : Integer;
                  BMnt : Integer;
                  FV   : IEEE_Float_64 := 0.0;
                  R    : constant String := Next_Reg (Ctx.E, "fl");
               begin
                  if not Is_Void_Tree (TS) and then KIND (TS) = DN_FLOAT
                    and then Has_Static_Val (D (SM_ACCURACY, TS))
                  then
                     Digs := Integer (Eval_Int64 (D (SM_ACCURACY, TS)));
                  end if;
                  if not Is_Void_Tree (BT) and then KIND (BT) = DN_FLOAT
                    and then Has_Static_Val (D (SM_ACCURACY, BT))
                  then
                     BDig := Integer (Eval_Int64 (D (SM_ACCURACY, BT)));
                  end if;
                  Mant := Integer (IEEE_Float_64'Ceiling (IEEE_Float_64 (Digs) * 3.32192809488736234787)) + 1;
                  BMnt := Integer (IEEE_Float_64'Ceiling (IEEE_Float_64 (BDig) * 3.32192809488736234787)) + 1;
                  if AName = "SMALL" then
                     FV := 2.0 ** (-(4 * Mant) - 1);
                  elsif AName = "SAFE_SMALL" then
                     declare
                        SE : constant Integer := (if Is32 then 125 else 1021);
                     begin
                        FV := 2.0 ** (-SE - 1);
                     end;
                  elsif AName = "LARGE" then
                     FV := (2.0 ** (4 * Mant)) * (1.0 - 2.0 ** (-Mant));
                  elsif AName = "SAFE_LARGE" then
                     declare
                        SE : constant Integer := (if Is32 then 125 else 1021);
                     begin
                        FV := (2.0 ** SE) * (1.0 - 2.0 ** (-BMnt));
                     end;
                  elsif AName = "EPSILON" then
                     FV := 2.0 ** (1 - Mant);
                  end if;
                  if PT_Str = "f32" or else PT_Str = "f64" then
                     Res.Kind := EK_Scalar;
                     Res.Reg := To_Unbounded_String (Emit_Float_Const_Safe (Ctx, FV, PT_Str, "fl"));
                     Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  elsif Is_Fixed_Type (ET) then
                     declare
                        FR  : constant String := Next_Reg (Ctx.E, "flf");
                     begin
                        Emit_Inst (Ctx.E, FR & " = const.f64 " & Format_Float64 (FV));
                        Res.Kind := EK_Scalar;
                        Res.Reg  := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, FR, "f64", ET, PT_Str));
                        Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  else
                     Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0");
                  end if;
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                  return Res;
               end;
            end;

         elsif AName = "MACHINE_ROUNDS" or else AName = "MACHINE_OVERFLOWS" then
            declare
               Pfx_TS : constant TREE := Subst_Resolve_Type (Ctx, Resolve_Prefix_Type (Pfx));
               Val : constant Integer :=
                 (if AName = "MACHINE_ROUNDS" and then not Is_Fixed_Type (Pfx_TS) then 1 else 0);
               R   : constant String := Next_Reg (Ctx.E, "mb");
            begin
               Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Val));
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "CONSTRAINED" then
            declare
               Is_Con : Boolean := True;
               PD     : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Pfx)));
               TS     : constant TREE := Resolve_Type_Spec (Resolve_Prefix_Type (Pfx));
            begin
               if not Is_Void_Tree (PD) and then KIND (PD) in CLASS_TYPE_NAME then
                  -- Type'CONSTRAINED (LRM 3.7.4(3)): False for unconstrained private/formal type
                  -- (with discriminants or generic formal private instantiated with unconstrained array/record)
                  declare
                     Raw_T : constant TREE := Resolve_Prefix_Type (Pfx);
                     Is_Formal_Sub : constant Boolean :=
                       not Is_Void_Tree (Raw_T) and then Ctx.Generic_Subst.Contains (Tree_Key (Raw_T));
                  begin
                     if not Is_Formal_Sub and then not Is_Void_Tree (Raw_T)
                       and then KIND (Raw_T) in DN_PRIVATE | DN_L_PRIVATE
                       and then (Is_Void_Tree (D (SM_DISCRIMINANT_S, Raw_T))
                                 or else IS_EMPTY (LIST (D (SM_DISCRIMINANT_S, Raw_T))))
                     then
                        Is_Con := True;
                     elsif not Is_Void_Tree (TS) and then KIND (TS) = DN_ARRAY then
                        Is_Con := False;
                     elsif not Is_Void_Tree (TS) and then KIND (TS) in DN_RECORD | DN_PRIVATE | DN_L_PRIVATE then
                        declare
                           DS : constant TREE := D (SM_DISCRIMINANT_S, TS);
                        begin
                           if not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS)) then
                              Is_Con := False;
                           end if;
                        end;
                     end if;
                  end;
               else
                  -- P3-3.3: Object'CONSTRAINED evaluates prefix when it may have side effects/null checks (e.g. P.all, F),
                  -- but NOT for simple constant names (e.g. deferred constant in package spec before private part: c74208b)!
                  declare
                     Unw_Pfx : TREE := Pfx;
                     PDef    : TREE;
                     Need_Eval : Boolean;
                  begin
                     while not Is_Void_Tree (Unw_Pfx) and then KIND (Unw_Pfx) = DN_PARENTHESIZED loop
                        Unw_Pfx := D (AS_EXP, Unw_Pfx);
                     end loop;
                     PDef := Get_Name_Defn (Unw_Pfx);
                     Need_Eval :=
                       KIND (Pfx) in DN_ALL | DN_FUNCTION_CALL | DN_NULLARY_CALL | DN_INDEXED | DN_SLICE
                                   | DN_SELECTED | DN_PARENTHESIZED | DN_CONVERSION | DN_QUALIFIED
                       and then (Is_Void_Tree (PDef) or else KIND (PDef) /= DN_CONSTANT_ID);
                     if Need_Eval then
                        declare
                           Dummy_CP : constant Expr_Val := Emit_LValue (Ctx, Pfx);
                           pragma Unreferenced (Dummy_CP);
                        begin
                           null;
                        end;
                     end if;
                     declare
                        Dyn_Con  : constant String := Emit_Actual_Constrained (Ctx, Pfx);
                        R_Con    : constant String := Emit_Int_Convert (Ctx, Dyn_Con, "i8", PT_Str);
                     begin
                        Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R_Con);
                        Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  end;
               end if;
               declare
                  R : constant String := Next_Reg (Ctx.E, "con");
               begin
                  Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & (if Is_Con then "1" else "0"));
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                  return Res;
               end;
            end;

         elsif AName = "POSITION" or else AName = "FIRST_BIT" or else AName = "LAST_BIT" then
            declare
               Dummy_Pfx : constant Expr_Val := Emit_LValue (Ctx, Pfx);
               pragma Unreferenced (Dummy_Pfx);
               Des  : TREE := Pfx;
               Def  : TREE := CONST_VOID;
               Val  : Integer_64 := 0;
               R    : constant String := Next_Reg (Ctx.E, "rcp");
            begin
               if KIND (Des) = DN_SELECTED then
                  declare
                     P_Node : constant TREE := D (AS_NAME, Des);
                     D_Node : constant TREE := D (AS_DESIGNATOR, Des);
                     PT     : constant TREE := Resolve_Type_Spec (Resolve_Prefix_Type (P_Node));
                     P_Rec  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (if Is_Access_Type (PT) then D (SM_DESIG_TYPE, PT) else PT));
                     Dummy  : Type_Info;
                  begin
                     if not Is_Void_Tree (P_Rec) then
                        Dummy := Get_Type_Info (Ctx, P_Rec);
                     end if;
                     Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D_Node)));
                     -- Check for explicit record representation clause (SM_COMP_REP_S / CD_COMP_UNIT_OFFSET)
                     if not Is_Void_Tree (Def) and then KIND (Def) in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                        declare
                           CK : constant Unsigned_32 := Tree_Key (Def);
                           Got_Rep : Boolean := False;
                        begin
                           if not Is_Void_Tree (P_Rec) and then KIND (P_Rec) = DN_RECORD then
                              declare
                                 RR : constant TREE := D (SM_REPRESENTATION, P_Rec);
                              begin
                                 if not Is_Void_Tree (RR) and then KIND (RR) = DN_RECORD_REP then
                                    declare
                                       CRS : constant TREE := D (AS_COMP_REP_S, RR);
                                       CRL : SEQ_TYPE := (if not Is_Void_Tree (CRS) then LIST (CRS) else MAKE);
                                       CR  : TREE;
                                    begin
                                       while not IS_EMPTY (CRL) loop
                                          POP (CRL, CR);
                                          if KIND (CR) = DN_COMP_REP then
                                             declare
                                                CN : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, CR))));
                                             begin
                                                if CN = Def then
                                                   declare
                                                      Pos_E : constant TREE := D (AS_EXP, CR);
                                                      Rng   : constant TREE := D (AS_RANGE, CR);
                                                      Pos_V : constant Integer_64 := (if Has_Static_Val (Safe_SM_Value (Pos_E)) then Eval_Int64 (Safe_SM_Value (Pos_E)) else 0);
                                                      FB_V  : constant Integer_64 := (if not Is_Void_Tree (Rng) and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, Rng))) then Eval_Int64 (Safe_SM_Value (D (AS_EXP1, Rng))) else 0);
                                                      LB_V  : constant Integer_64 := (if not Is_Void_Tree (Rng) and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, Rng))) then Eval_Int64 (Safe_SM_Value (D (AS_EXP2, Rng))) else 7);
                                                      Adj   : constant Integer_64 := (if FB_V >= 0 then FB_V / 8 else 0);
                                                   begin
                                                      if AName = "POSITION" then Val := Pos_V + Adj;
                                                      elsif AName = "FIRST_BIT" then Val := FB_V - Adj * 8;
                                                      else Val := LB_V - Adj * 8;
                                                      end if;
                                                      Got_Rep := True;
                                                   end;
                                                   exit;
                                                end if;
                                             end;
                                          end if;
                                       end loop;
                                    end;
                                 end if;
                              end;
                           end if;
                           if not Got_Rep and then Ctx.Fields.Contains (CK) then
                              declare
                                 FE : constant Field_Entry := Ctx.Fields.Element (CK);
                              begin
                                 if AName = "POSITION" then
                                    Val := Integer_64 (FE.Byte_Offset);
                                 elsif AName = "FIRST_BIT" then
                                    Val := Integer_64 (if FE.Bit_Size > 0 then FE.Bit_Offset else 0);
                                 else
                                    Val := Integer_64 (if FE.Bit_Size > 0 then FE.Bit_Offset + FE.Bit_Size - 1
                                                       else Natural'Max (1, FE.Field_Size) * 8 - 1);
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                  end;
               end if;
               Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Long_Long_Integer (Val)));
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "CALLABLE" or else AName = "TERMINATED" then
            declare
               TV   : constant Expr_Val := Emit_Expr (Ctx, Pfx);
               TReg : Unbounded_String := TV.Reg;
               Fn   : constant String := (if AName = "CALLABLE" then "@__ada_task_callable" else "@__ada_task_terminated");
               RV   : Unbounded_String;
            begin
               if Is_Access_Type (TV.Ada_Type) then
                  -- Access-to-task: null-check and load task handle from heap box (c34007j, cd2a83g)
                  declare
                     TH : constant String := Next_Reg (Ctx.E, "ath");
                  begin
                     Emit_Null_Check (Ctx, To_String (TReg));
                     Emit_Inst (Ctx.E, TH & " = load.ptr " & To_String (TReg) & ", align 8");
                     TReg := To_Unbounded_String (TH);
                  end;
               end if;
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (TReg), "i8", RV, Nounwind => True);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i8"); Res.Ada_Type := ET; Res.Align := 1;
               return Res;
            end;

         elsif AName = "COUNT" then
            declare
               E_Node : TREE := Pfx;
               Fam_E  : TREE := CONST_VOID;
               E_Def  : TREE;
               E_Idx  : Unbounded_String := To_Unbounded_String ("0");
               Self_R : Unbounded_String := Ctx.Cur_Task_Reg;
               RV     : Unbounded_String;
            begin
               if KIND (E_Node) = DN_INDEXED then
                  declare
                     EL : SEQ_TYPE := LIST (D (AS_EXP_S, E_Node));
                  begin
                     if not IS_EMPTY (EL) then POP (EL, Fam_E); end if;
                  end;
                  E_Node := D (AS_NAME, E_Node);
               elsif KIND (E_Node) = DN_FUNCTION_CALL then
                  declare
                     EL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, E_Node));
                  begin
                     if not IS_EMPTY (EL) then
                        POP (EL, Fam_E);
                        if KIND (Fam_E) = DN_ASSOC or else KIND (Fam_E) = DN_NAMED then
                           Fam_E := D (AS_EXP, Fam_E);
                        end if;
                     end if;
                  end;
                  E_Node := D (AS_NAME, E_Node);
               end if;
               E_Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (E_Node)));
               declare
                  Target_TK : Unsigned_32 := 0;
               begin
                  if Ctx.Entries.Contains (Tree_Key (E_Def)) then
                     declare
                        EI : constant Entry_Info := Ctx.Entries.Element (Tree_Key (E_Def));
                     begin
                        Target_TK := EI.Task_Key;
                        E_Idx := To_Unbounded_String (Emit_Entry_Index_Reg (Ctx, EI, Fam_E));
                     end;
                  end if;
                  Self_R := To_Unbounded_String (Emit_Enclosing_Task_Self_By_Key (Ctx, Target_TK));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_entry_count", To_String (Self_R) & ", " & To_String (E_Idx), "i32", RV, Nounwind => True);
               end;
               Res.Kind := EK_Scalar;
               Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", PT_Str));
               Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "FORE" or else AName = "AFT" then
            declare
               TS  : constant TREE := Subst_Resolve_Type (Ctx, Resolve_Prefix_Type (Pfx));
               BT  : constant TREE := Subst_Base_Type (Ctx, TS);
            begin
               if AName = "AFT" then
                  declare
                     Del_T : constant TREE := (if not Is_Void_Tree (TS) and then KIND (TS) = DN_FIXED then D (SM_ACCURACY, TS) else CONST_VOID);
                     Del   : constant IEEE_Float_64 := Fixed_Delta (TS);
                     N     : Integer := 1;
                     R     : constant String := Next_Reg (Ctx.E, "fa");
                     Exact : Boolean := False;
                  begin
                     if not Is_Void_Tree (Del_T) and then KIND (Del_T) = DN_REAL_VAL then
                        declare
                           DN, DD : Integer_64;
                           ON, OD : Boolean;
                        begin
                           Eval_Int64_Checked (D (XD_NUMER, Del_T), DN, ON);
                           Eval_Int64_Checked (D (XD_DENOM, Del_T), DD, OD);
                           if not ON and then not OD and then DN > 0 and then DD > 0 then
                              declare
                                 UN : Unsigned_64 := Unsigned_64 (DN);
                                 UD : constant Unsigned_64 := Unsigned_64 (DD);
                              begin
                                 Exact := True;
                                 if UN <= Unsigned_64'Last / 10 then
                                    UN := UN * 10;
                                    while N < 38 and then UN < UD loop
                                       N := N + 1;
                                       if UN > Unsigned_64'Last / 10 then
                                          exit;
                                       end if;
                                       UN := UN * 10;
                                    end loop;
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                     if not Exact then
                        declare
                           P10 : IEEE_Float_64 := 10.0;
                        begin
                           while N < 38 and then (P10 * Del) < (1.0 - 1.0E-14) loop
                              N := N + 1;
                              P10 := P10 * 10.0;
                           end loop;
                        end;
                     end if;
                     Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (N));
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  end;
               else
                  declare
                     Rng   : constant TREE := (if not Is_Void_Tree (TS) then D (SM_RANGE, TS) else CONST_VOID);
                     BSm   : constant IEEE_Float_64 := Fixed_Small (BT);
                     Sm_Ok : Boolean;
                     Sm_N, Sm_D : Integer_64;
                     Lo64, Hi64 : Unbounded_String;
                     RV    : Unbounded_String;
                     SR_F  : Saved_Range_Info;
                  begin
                     Fixed_Small_Rational (BT, Sm_Ok, Sm_N, Sm_D);
                     if Try_Lookup_Saved_Range (Ctx, TS, SR_F) then
                        declare
                           L0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_F.Lo_Ptr), To_String (SR_F.Pir_Type), SR_F.Align, SR_F.Is_Global);
                           H0 : constant String := Load_Saved_Slot (Ctx, To_String (SR_F.Hi_Ptr), To_String (SR_F.Pir_Type), SR_F.Align, SR_F.Is_Global);
                        begin
                           Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, L0, To_String (SR_F.Pir_Type), "i64"));
                           Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, H0, To_String (SR_F.Pir_Type), "i64"));
                        end;
                     elsif not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                        Lo64 := To_Unbounded_String (Emit_Fixed_Bound_I64 (D (AS_EXP1, Rng), BT));
                        Hi64 := To_Unbounded_String (Emit_Fixed_Bound_I64 (D (AS_EXP2, Rng), BT));
                     else
                        declare
                           R : constant String := Next_Reg (Ctx.E, "ff");
                        begin
                           Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 2");
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     end if;
                     if Sm_Ok then
                        Emit_Call_Or_Invoke (Ctx, "@__ada_fixed_fore_rat",
                          To_String (Lo64) & ", " & To_String (Hi64) & ", " &
                          Img (Long_Long_Integer (Sm_N)) & ", " & Img (Long_Long_Integer (Sm_D)), "i32", RV, Nounwind => True);
                     else
                        declare
                           BSR : constant String := Next_Reg (Ctx.E, "ffbs");
                        begin
                           Emit_Inst (Ctx.E, BSR & " = const.f64 " & Format_Float64 (BSm));
                           Emit_Call_Or_Invoke (Ctx, "@__ada_fixed_fore",
                             To_String (Lo64) & ", " & To_String (Hi64) & ", " & BSR, "i32", RV, Nounwind => True);
                        end;
                     end if;
                     Res.Kind := EK_Scalar;
                     Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", PT_Str));
                     Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  end;
               end if;
            end;

         elsif AName = "STORAGE_SIZE" then
            declare
               PD_SS : constant TREE := Get_Name_Defn (Pfx);
            begin
               if not Is_Type_Prefix (Pfx)
                 and then not (not Is_Void_Tree (PD_SS) and then KIND (PD_SS) in CLASS_TYPE_NAME)
               then
                  declare
                     Dummy_SS : constant Expr_Val := Emit_LValue (Ctx, Pfx);
                     pragma Unreferenced (Dummy_SS);
                  begin
                     null;
                  end;
               end if;
            end;
            declare
               TS   : constant TREE := Resolve_Type_Spec (Resolve_Prefix_Type (Pfx));
               BT   : constant TREE := Base_Type_Spec (TS);
               SS_E : TREE := CONST_VOID;
               R    : constant String := Next_Reg (Ctx.E, "ssz");
            begin
               if not Is_Void_Tree (TS) and then (KIND (TS) = DN_ACCESS or else KIND (TS) = DN_TASK_SPEC) then
                  SS_E := D (SM_STORAGE_SIZE, TS);
               end if;
               if Is_Void_Tree (SS_E) and then not Is_Void_Tree (BT)
                 and then (KIND (BT) = DN_ACCESS or else KIND (BT) = DN_TASK_SPEC)
               then
                  declare
                     Cur : TREE := BT;
                  begin
                     for Step in 1 .. 16 loop
                        exit when Is_Void_Tree (Cur) or else KIND (Cur) not in DN_ACCESS | DN_TASK_SPEC;
                        SS_E := D (SM_STORAGE_SIZE, Cur);
                        exit when not Is_Void_Tree (SS_E);
                        Cur := Resolve_Type_Spec (D (SM_DERIVED, Cur));
                     end loop;
                  end;
               end if;
               if not Is_Void_Tree (SS_E) then
                  -- P3-6.6: Load cross-unit library-level 'STORAGE_SIZE global when not pre-saved in current unit
                  declare
                     Owning_T : TREE := (if not Is_Void_Tree (TS) and then not Is_Void_Tree (D (SM_STORAGE_SIZE, TS)) then TS else BT);
                  begin
                     if not Is_Void_Tree (Owning_T) and then KIND (Owning_T) in DN_ACCESS | DN_TASK_SPEC then
                        declare
                           Cur_O : TREE := Owning_T;
                        begin
                           for Step in 1 .. 16 loop
                              exit when Is_Void_Tree (Cur_O) or else KIND (Cur_O) not in DN_ACCESS | DN_TASK_SPEC;
                              exit when not Is_Void_Tree (D (SM_STORAGE_SIZE, Cur_O));
                              Cur_O := Resolve_Type_Spec (D (SM_DERIVED, Cur_O));
                           end loop;
                           if not Is_Void_Tree (Cur_O) and then not Ctx.Saved_Expr_Vals.Contains (Tree_Key (SS_E))
                             and then not Has_Static_Val (Safe_SM_Value (SS_E))
                             and then not Is_Void_Tree (D (XD_SOURCE_NAME, Cur_O))
                             and then Is_Library_Level (D (XD_SOURCE_NAME, Cur_O))
                           then
                              declare
                                 SSym : constant String := "@ada_ssz__" & Qualified_Ada_Name (D (XD_SOURCE_NAME, Cur_O));
                                 GA   : constant String := Next_Reg (Ctx.E, "ssga");
                                 GV   : constant String := Next_Reg (Ctx.E, "ssgv");
                                 CR   : Unbounded_String;
                              begin
                                 Add_Global_Decl (Ctx.E, SSym, "extern " & SSym & " = global i64, align 8");
                                 Emit_Inst (Ctx.E, GA & " = addr " & SSym);
                                 Emit_Inst (Ctx.E, GV & " = load.i64 " & GA & ", align 8");
                                 CR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, GV, "i64", PT_Str));
                                 Res.Kind := EK_Scalar; Res.Reg := CR;
                                 Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                 return Res;
                              end;
                           end if;
                        end;
                     end if;
                  end;
                  declare
                     SV : constant Expr_Val := Emit_Expr (Ctx, SS_E, ET);
                     CR : constant String := Emit_Scalar_Coerce (Ctx, To_String (SV.Reg), To_String (SV.Pir_Type), PT_Str);
                  begin
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (CR);
                     Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  end;
               end if;
               Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 65536");
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         else
            declare
               R : constant String := Next_Reg (Ctx.E, "au");
            begin
               if PT_Str = "f32" or else PT_Str = "f64" then
                  Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0.0");
               else
                  Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0");
               end if;
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;
         end if;
      end;
   end Emit_Attribute;

end Cg_Expr.Attr;
