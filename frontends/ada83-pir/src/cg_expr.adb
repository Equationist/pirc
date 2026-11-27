-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr.Attr; use Cg_Expr.Attr;
with Cg_Expr.Call;
with Cg_Expr.Composite;

package body Cg_Expr is

   -- Format an Integer_64 immediate guaranteed to fit in Pir_Ty's verifier range (signed or unsigned).
   -- Preserves exact value whenever in range; otherwise wraps two's-complement to match trunc.<Pir_Ty>.
   function Format_Int_Imm_For_Pir (Val : Integer_64; Pir_Ty : String) return String is
   begin
      if Pir_Ty = "i32" then
         if Val >= -2_147_483_648 and then Val <= 4_294_967_295 then
            return Img (Long_Long_Integer (Val));
         end if;
         declare
            U : constant Unsigned_64 := Unsigned_64'Mod (Val) and 16#FFFF_FFFF#;
            I : constant Long_Long_Integer :=
              (if U >= 16#8000_0000# then Long_Long_Integer (U) - 16#1_0000_0000#
               else Long_Long_Integer (U));
         begin
            return Img (I);
         end;
      elsif Pir_Ty = "i16" then
         if Val >= -32_768 and then Val <= 65_535 then
            return Img (Long_Long_Integer (Val));
         end if;
         declare
            U : constant Unsigned_64 := Unsigned_64'Mod (Val) and 16#FFFF#;
            I : constant Long_Long_Integer :=
              (if U >= 16#8000# then Long_Long_Integer (U) - 16#1_0000#
               else Long_Long_Integer (U));
         begin
            return Img (I);
         end;
      elsif Pir_Ty = "i8" then
         if Val >= -128 and then Val <= 255 then
            return Img (Long_Long_Integer (Val));
         end if;
         declare
            U : constant Unsigned_64 := Unsigned_64'Mod (Val) and 16#FF#;
            I : constant Long_Long_Integer :=
              (if U >= 16#80# then Long_Long_Integer (U) - 256
               else Long_Long_Integer (U));
         begin
            return Img (I);
         end;
      end if;
      return Img (Long_Long_Integer (Val));
   end Format_Int_Imm_For_Pir;

   function Emit_Int_Const
     (Ctx    : in out Context;
      Val    : Integer_64;
      Pir_Ty : String;
      Prefix : String := "c") return String
   is
      R : constant String := Next_Reg (Ctx.E, Prefix);
   begin
      Emit_Inst (Ctx.E, R & " = const." & Pir_Ty & " " & Format_Int_Imm_For_Pir (Val, Pir_Ty));
      return R;
   end Emit_Int_Const;

   function Int_Bits_Of_Pir (Pir_Ty : String) return Natural is
   begin
      if Pir_Ty = "i8" then return 8;
      elsif Pir_Ty = "i16" then return 16;
      elsif Pir_Ty = "i32" then return 32;
      else return 64;
      end if;
   end Int_Bits_Of_Pir;

   function Min_Signed_For_Pir (Pir_Ty : String) return Long_Long_Integer is
   begin
      if Pir_Ty = "i8" then return -128;
      elsif Pir_Ty = "i16" then return -32768;
      elsif Pir_Ty = "i32" then return -2147483648;
      else return Long_Long_Integer'First;
      end if;
   end Min_Signed_For_Pir;

   function Emit_Int_Convert
     (Ctx         : in out Context;
      Val_Reg     : String;
      From_Ty     : String;
      To_Ty       : String;
      Is_Unsigned : Boolean := False) return String
   is
      FB : constant Natural := Int_Bits_Of_Pir (From_Ty);
      TB : constant Natural := Int_Bits_Of_Pir (To_Ty);
   begin
      if From_Ty = To_Ty then
         return Val_Reg;
      elsif From_Ty = "ptr" then
         declare
            R : constant String := Next_Reg (Ctx.E, "p2i");
         begin
            Emit_Inst (Ctx.E, R & " = ptrtoint." & To_Ty & " " & Val_Reg);
            return R;
         end;
      elsif To_Ty = "ptr" then
         declare
            R : constant String := Next_Reg (Ctx.E, "i2p");
         begin
            Emit_Inst (Ctx.E, R & " = inttoptr.ptr " & Val_Reg);
            return R;
         end;
      elsif TB > FB then
         declare
            R : constant String := Next_Reg (Ctx.E, "ext");
            Op : constant String := (if Is_Unsigned then "zext." else "sext.");
         begin
            Emit_Inst (Ctx.E, R & " = " & Op & To_Ty & " " & Val_Reg);
            return R;
         end;
      else
         declare
            R : constant String := Next_Reg (Ctx.E, "tr");
         begin
            Emit_Inst (Ctx.E, R & " = trunc." & To_Ty & " " & Val_Reg);
            return R;
         end;
      end if;
   end Emit_Int_Convert;

   function Emit_Scalar_Coerce
     (Ctx         : in out Context;
      Val_Reg     : String;
      From_Ty     : String;
      To_Ty       : String;
      Is_Unsigned : Boolean := False) return String
   is
   begin
      if From_Ty = To_Ty then
         return Val_Reg;
      elsif To_Ty = "f64" and then From_Ty = "f32" then
         declare
            R : constant String := Next_Reg (Ctx.E, "fe");
         begin
            Emit_Inst (Ctx.E, R & " = fpext.f64 " & Val_Reg);
            return R;
         end;
      elsif To_Ty = "f32" and then From_Ty = "f64" then
         declare
            R : constant String := Next_Reg (Ctx.E, "ft");
         begin
            Emit_Inst (Ctx.E, R & " = fptrunc.f32 " & Val_Reg);
            return R;
         end;
      elsif (To_Ty = "f64" or else To_Ty = "f32") and then From_Ty /= "ptr" then
         declare
            R  : constant String := Next_Reg (Ctx.E, "i2f");
            Op : constant String := (if Is_Unsigned then "uitofp." else "sitofp.");
         begin
            Emit_Inst (Ctx.E, R & " = " & Op & To_Ty & " " & Val_Reg);
            return R;
         end;
      elsif (From_Ty = "f64" or else From_Ty = "f32") and then To_Ty /= "ptr" then
         declare
            F64  : Unbounded_String := To_Unbounded_String (Val_Reg);
            V64  : Unbounded_String;
            Chk  : constant String := (if Ctx.Enable_Checks then "1" else "0");
            MinB : constant Long_Long_Integer := (if Is_Unsigned then 0 else Min_Signed_For_Pir (To_Ty));
            MaxB : constant Long_Long_Integer :=
              (if Is_Unsigned and then To_Ty = "i8" then 255
               elsif Is_Unsigned and then To_Ty = "i16" then 65535
               elsif To_Ty = "i8" then 127 elsif To_Ty = "i16" then 32767
               elsif To_Ty = "i32" then 2147483647 else Long_Long_Integer'Last);
         begin
            if From_Ty = "f32" then
               declare
                  Ex : constant String := Next_Reg (Ctx.E, "f2ie");
               begin
                  Emit_Inst (Ctx.E, Ex & " = fpext.f64 " & Val_Reg);
                  F64 := To_Unbounded_String (Ex);
               end;
            end if;
            Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
              To_String (F64) & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
            return Emit_Int_Convert (Ctx, To_String (V64), "i64", To_Ty);
         end;
      else
         return Emit_Int_Convert (Ctx, Val_Reg, From_Ty, To_Ty, Is_Unsigned => Is_Unsigned);
      end if;
   end Emit_Scalar_Coerce;

   function Emit_F64_To_Fixed_Ticks
     (Ctx     : in out Context;
      F_Reg   : String;
      F_Pir   : String;
      Fixed_T : TREE;
      Dst_Pir : String) return String
   is
      F64  : Unbounded_String := To_Unbounded_String (F_Reg);
      Div  : constant String := Next_Reg (Ctx.E, "f2xd");
      V64  : Unbounded_String;
      Sm   : constant IEEE_Float_64 := Fixed_Small (Fixed_T);
      Chk  : constant String := (if Ctx.Enable_Checks then "1" else "0");
      MinB : constant Long_Long_Integer := Min_Signed_For_Pir (Dst_Pir);
      MaxB : constant Long_Long_Integer :=
        (if Dst_Pir = "i8" then 127 elsif Dst_Pir = "i16" then 32767
         elsif Dst_Pir = "i32" then 2147483647 else Long_Long_Integer'Last);
   begin
      if F_Pir = "f32" then
         declare
            Ex : constant String := Next_Reg (Ctx.E, "f2xe");
         begin
            Emit_Inst (Ctx.E, Ex & " = fpext.f64 " & F_Reg);
            F64 := To_Unbounded_String (Ex);
         end;
      end if;
      Emit_Inst (Ctx.E, Div & " = fdiv.f64 " & To_String (F64) & ", " & Format_Float64 (Sm));
      Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
        Div & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
      return Emit_Int_Convert (Ctx, To_String (V64), "i64", Dst_Pir);
   end Emit_F64_To_Fixed_Ticks;

   procedure Emit_Check_Fail_If (Ctx : in out Context; Cond_Reg : String; Tag : String := "") is
      Fail_L : constant String := Next_Label (Ctx.E, "chk_fail");
      Ok_L   : constant String := Next_Label (Ctx.E, "chk_ok");
   begin
      Emit_Term (Ctx.E, "br " & Cond_Reg & ", " & Fail_L & ", " & Ok_L);
      Emit_Label (Ctx.E, Fail_L);
      if Tag'Length > 0 then
         Emit_Raise_Tag (Ctx, Tag);
      else
         Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
      end if;
      Emit_Label (Ctx.E, Ok_L);
   end Emit_Check_Fail_If;

   -- Helper: load a pre-evaluated scalar from a saved global or hoisted entry alloca
   function Load_Saved_Slot
     (Ctx     : in out Context;
      Ptr_Str : String;
      Pir_Ty  : String;
      Align   : Natural;
      Is_Glob : Boolean) return String
   is
      V : constant String := Next_Reg (Ctx.E, "svl");
   begin
      if Is_Glob then
         declare
            A : constant String := Next_Reg (Ctx.E, "sva");
         begin
            Emit_Inst (Ctx.E, A & " = addr " & Ptr_Str);
            Emit_Inst (Ctx.E, V & " = load." & Pir_Ty & " " & A & ", align " & Img (Natural'Max (1, Align)));
         end;
      else
         Emit_Inst (Ctx.E, V & " = load." & Pir_Ty & " " & Ptr_Str & ", align " & Img (Natural'Max (1, Align)));
      end if;
      return V;
   end Load_Saved_Slot;

   procedure Save_Scalar_Slot
     (Ctx         : in out Context;
      Val_Reg     : String;
      Pir_Ty      : String;
      Align       : Natural;
      Ptr_Out     : out Unbounded_String;
      Is_Glob_Out : out Boolean)
   is
      Al : constant Natural := Natural'Max (1, Align);
   begin
      if Ctx.Cur_Subp_Key = 0 then
         declare
            GSym : constant String := "@ada_sv_" & Img (Next_Sym_Id (Ctx.E));
            A    : constant String := Next_Reg (Ctx.E, "sva");
         begin
            Add_Global_Decl (Ctx.E, GSym, GSym & " = global " & Pir_Ty & " = zeroinit, align " & Img (Al));
            Emit_Inst (Ctx.E, A & " = addr " & GSym);
            Emit_Inst (Ctx.E, "store." & Pir_Ty & " " & Val_Reg & ", " & A & ", align " & Img (Al));
            Ptr_Out     := To_Unbounded_String (GSym);
            Is_Glob_Out := True;
         end;
      else
         declare
            Slot : constant String := Next_Reg (Ctx.E, "sv");
         begin
            Emit_Entry_Alloca (Ctx.E, Slot & " = alloca " & Pir_Ty & ", align " & Img (Al));
            Emit_Inst (Ctx.E, "store." & Pir_Ty & " " & Val_Reg & ", " & Slot & ", align " & Img (Al));
            Ptr_Out     := To_Unbounded_String (Slot);
            Is_Glob_Out := False;
         end;
      end if;
   end Save_Scalar_Slot;

   procedure Emit_Range_Check
     (Ctx        : in out Context;
      Val_Reg    : String;
      Val_Pir_Ty : String;
      Sub_Type   : TREE)
   is
      R  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Sub_Type));
      St : Boolean;
      Lo, Hi : Integer_64;
   begin
      if not Ctx.Enable_Checks or else Is_Void_Tree (R) or else Val_Pir_Ty = "ptr" or else Is_Access_Type (R) then
         return;
      end if;
      -- Reuse pre-evaluated dynamic scalar bounds saved at subtype/object elaboration (c32001a, c32107a)
      if Ctx.Saved_Type_Ranges.Contains (Tree_Key (R)) then
         declare
            SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (R));
         begin
            if SR.Is_Global or else SR.Owner_Subp = Ctx.Cur_Subp_Key then
               declare
                  LR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                  HR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                  SR_U : constant Boolean := Is_Unsigned_Scalar (R, To_String (SR.Pir_Type));
                  L_R : constant String := Emit_Scalar_Coerce (Ctx, LR0, To_String (SR.Pir_Type), Val_Pir_Ty, Is_Unsigned => SR_U);
                  H_R : constant String := Emit_Scalar_Coerce (Ctx, HR0, To_String (SR.Pir_Type), Val_Pir_Ty, Is_Unsigned => SR_U);
                  C1  : constant String := Next_Reg (Ctx.E, "srlo");
                  C2  : constant String := Next_Reg (Ctx.E, "srhi");
                  Bad : constant String := Next_Reg (Ctx.E, "srbad");
               begin
                  if Val_Pir_Ty = "f32" or else Val_Pir_Ty = "f64" then
                     declare
                        InR : constant String := Next_Reg (Ctx.E, "srin");
                     begin
                        Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                        Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                        Emit_Inst (Ctx.E, Bad & " = xor.i8 " & InR & ", 1");
                     end;
                  else
                     declare
                        Is_U  : constant Boolean := Is_Unsigned_Scalar (R, Val_Pir_Ty);
                        Lt_Op : constant String := (if Is_U then "cmp_lt_u." else "cmp_lt_s.");
                        Gt_Op : constant String := (if Is_U then "cmp_gt_u." else "cmp_gt_s.");
                     begin
                        Emit_Inst (Ctx.E, C1 & " = " & Lt_Op & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = " & Gt_Op & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                        Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                     end;
                  end if;
                  Emit_Check_Fail_If (Ctx, Bad);
                  return;
               end;
            end if;
         end;
      end if;
      if KIND (R) = DN_FIXED then
         declare
            Rng : constant TREE := D (SM_RANGE, R);
            Sm  : constant IEEE_Float_64 := Fixed_Small (R);
         begin
            if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
               declare
                  E1 : constant TREE := D (AS_EXP1, Rng);
                  E2 : constant TREE := D (AS_EXP2, Rng);
                  V1 : constant TREE := Safe_SM_Value (E1);
                  V2 : constant TREE := Safe_SM_Value (E2);
                  Lo_S, Hi_S : Unbounded_String;
                  C1  : constant String := Next_Reg (Ctx.E, "fxlo");
                  C2  : constant String := Next_Reg (Ctx.E, "fxhi");
                  Bad : constant String := Next_Reg (Ctx.E, "fxbad");
               begin
                  if Has_Static_Val (V1) and then Has_Static_Val (V2) and then Sm > 0.0 then
                     declare
                        I1, I2 : Integer_64;
                        O1, O2 : Boolean;
                     begin
                        Eval_Fixed_Ticks_Exact_Checked (V1, R, I1, O1);
                        Eval_Fixed_Ticks_Exact_Checked (V2, R, I2, O2);
                        if I1 > Integer_64'First or else I2 < Integer_64'Last then
                           Lo_S := To_Unbounded_String (Img (Long_Long_Integer (I1)));
                           Hi_S := To_Unbounded_String (Img (Long_Long_Integer (I2)));
                           Emit_Inst (Ctx.E, C1 & " = cmp_lt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & To_String (Lo_S));
                           Emit_Inst (Ctx.E, C2 & " = cmp_gt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & To_String (Hi_S));
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end if;
                     end;
                  else
                     declare
                        EV1 : constant Expr_Val := Emit_Expr (Ctx, E1, R);
                        EV2 : constant Expr_Val := Emit_Expr (Ctx, E2, R);
                        L_R, H_R : Unbounded_String;
                     begin
                        if To_String (EV1.Pir_Type) = "f32" or else To_String (EV1.Pir_Type) = "f64" then
                           L_R := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), R, Val_Pir_Ty));
                        else
                           L_R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), Val_Pir_Ty));
                        end if;
                        if To_String (EV2.Pir_Type) = "f32" or else To_String (EV2.Pir_Type) = "f64" then
                           H_R := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (EV2.Reg), To_String (EV2.Pir_Type), R, Val_Pir_Ty));
                        else
                           H_R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (EV2.Reg), To_String (EV2.Pir_Type), Val_Pir_Ty));
                        end if;
                        Emit_Inst (Ctx.E, C1 & " = cmp_lt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & To_String (L_R));
                        Emit_Inst (Ctx.E, C2 & " = cmp_gt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & To_String (H_R));
                        Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end if;
               end;
            end if;
         end;
         return;
      elsif KIND (R) = DN_FLOAT then
         declare
            Rng : constant TREE := D (SM_RANGE, R);
         begin
            if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
               declare
                  E1 : constant TREE := D (AS_EXP1, Rng);
                  E2 : constant TREE := D (AS_EXP2, Rng);
                  V1 : constant TREE := Safe_SM_Value (E1);
                  V2 : constant TREE := Safe_SM_Value (E2);
                  C1  : constant String := Next_Reg (Ctx.E, "fllo");
                  C2  : constant String := Next_Reg (Ctx.E, "flhi");
                  Bad : constant String := Next_Reg (Ctx.E, "flbad");
               begin
                  if Has_Static_Val (V1) and then Has_Static_Val (V2) then
                     declare
                        F1_Raw : constant IEEE_Float_64 := Eval_Float64 (V1);
                        F2_Raw : constant IEEE_Float_64 := Eval_Float64 (V2);
                        -- P3-2.7: when checking f64 value against an f32 subtype, round static bounds to f32 first
                        -- so S(Long_Float(S'Last)) does not fail against f64(0.1) != f64(f32(0.1))!
                        F1  : constant IEEE_Float_64 :=
                          (if Scalar_Bits (R) <= 32 and then Val_Pir_Ty = "f64"
                           then IEEE_Float_64 (IEEE_Float_32 (F1_Raw)) else F1_Raw);
                        F2  : constant IEEE_Float_64 :=
                          (if Scalar_Bits (R) <= 32 and then Val_Pir_Ty = "f64"
                           then IEEE_Float_64 (IEEE_Float_32 (F2_Raw)) else F2_Raw);
                        L_C : constant String := Next_Reg (Ctx.E, "fllc");
                        H_C : constant String := Next_Reg (Ctx.E, "flhc");
                        InR : constant String := Next_Reg (Ctx.E, "flin");
                     begin
                        Emit_Inst (Ctx.E, L_C & " = const." & Val_Pir_Ty & " " & Format_Float64 (F1));
                        Emit_Inst (Ctx.E, H_C & " = const." & Val_Pir_Ty & " " & Format_Float64 (F2));
                        Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & Val_Pir_Ty & " " & Val_Reg & ", " & L_C);
                        Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & Val_Pir_Ty & " " & Val_Reg & ", " & H_C);
                        Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                        Emit_Inst (Ctx.E, Bad & " = xor.i8 " & InR & ", 1");
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  else
                     declare
                        EV1 : constant Expr_Val := Emit_Expr (Ctx, E1, R);
                        EV2 : constant Expr_Val := Emit_Expr (Ctx, E2, R);
                        L_R : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), Val_Pir_Ty);
                        H_R : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV2.Reg), To_String (EV2.Pir_Type), Val_Pir_Ty);
                        InR : constant String := Next_Reg (Ctx.E, "flin");
                     begin
                        Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                        Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                        Emit_Inst (Ctx.E, Bad & " = xor.i8 " & InR & ", 1");
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end if;
               end;
            end if;
         end;
         return;
      elsif not Is_Discrete_Type (R) then
         return;
      end if;
      Get_Discrete_Bounds (R, St, Lo, Hi);
      if not St then
         -- Dynamic range check if DN_RANGE exists
         if KIND (R) = DN_INTEGER or else KIND (R) = DN_ENUMERATION then
            declare
               Rng : constant TREE := D (SM_RANGE, R);
            begin
               if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                  declare
                     E1 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng));
                     E2 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng));
                     U1 : constant Boolean := Is_Unsigned_Scalar (E1.Ada_Type, To_String (E1.Pir_Type));
                     U2 : constant Boolean := Is_Unsigned_Scalar (E2.Ada_Type, To_String (E2.Pir_Type));
                     L_R : constant String := Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), Val_Pir_Ty, Is_Unsigned => U1);
                     H_R : constant String := Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), Val_Pir_Ty, Is_Unsigned => U2);
                     C1  : constant String := Next_Reg (Ctx.E, "rlo");
                     C2  : constant String := Next_Reg (Ctx.E, "rhi");
                     Bad : constant String := Next_Reg (Ctx.E, "rbad");
                     Is_U  : constant Boolean := Is_Unsigned_Scalar (R, Val_Pir_Ty);
                     Lt_Op : constant String := (if Is_U then "cmp_lt_u." else "cmp_lt_s.");
                     Gt_Op : constant String := (if Is_U then "cmp_gt_u." else "cmp_gt_s.");
                  begin
                     Emit_Inst (Ctx.E, C1 & " = " & Lt_Op & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                     Emit_Inst (Ctx.E, C2 & " = " & Gt_Op & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               elsif not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE_ATTRIBUTE then
                  declare
                     RT_U, Lo_U, Hi_U : Unbounded_String;
                  begin
                     Emit_Discrete_Range (Ctx, Rng, RT_U, Lo_U, Hi_U);
                     declare
                        RU  : constant Boolean := Is_Unsigned_Scalar (R, To_String (RT_U));
                        L_R : constant String := Emit_Int_Convert (Ctx, To_String (Lo_U), To_String (RT_U), Val_Pir_Ty, Is_Unsigned => RU);
                        H_R : constant String := Emit_Int_Convert (Ctx, To_String (Hi_U), To_String (RT_U), Val_Pir_Ty, Is_Unsigned => RU);
                        C1  : constant String := Next_Reg (Ctx.E, "rlo");
                        C2  : constant String := Next_Reg (Ctx.E, "rhi");
                        Bad : constant String := Next_Reg (Ctx.E, "rbad");
                        Is_U  : constant Boolean := Is_Unsigned_Scalar (R, Val_Pir_Ty);
                        Lt_Op : constant String := (if Is_U then "cmp_lt_u." else "cmp_lt_s.");
                        Gt_Op : constant String := (if Is_U then "cmp_gt_u." else "cmp_gt_s.");
                     begin
                        Emit_Inst (Ctx.E, C1 & " = " & Lt_Op & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = " & Gt_Op & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                        Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end;
               end if;
            end;
         end if;
         return;
      end if;

      -- Skip tautological full-width checks (e.g. full Integer range on i32)
      declare
         Bits    : constant Natural := Int_Bits_Of_Pir (Val_Pir_Ty);
         Is_U    : constant Boolean := Is_Unsigned_Scalar (R, Val_Pir_Ty);
         Lt_Op   : constant String := (if Is_U then "cmp_lt_u." else "cmp_lt_s.");
         Gt_Op   : constant String := (if Is_U then "cmp_gt_u." else "cmp_gt_s.");
         Full_Lo : Integer_64;
         Full_Hi : Integer_64;
      begin
         if Is_U then
            case Bits is
               when 8  => Full_Lo := 0; Full_Hi := 255;
               when 16 => Full_Lo := 0; Full_Hi := 65535;
               when 32 => Full_Lo := 0; Full_Hi := 4294967295;
               when others => Full_Lo := 0; Full_Hi := Integer_64'Last;
            end case;
         else
            case Bits is
               when 8  => Full_Lo := -128; Full_Hi := 127;
               when 16 => Full_Lo := -32768; Full_Hi := 32767;
               when 32 => Full_Lo := -2147483648; Full_Hi := 2147483647;
               when others => Full_Lo := Integer_64'First; Full_Hi := Integer_64'Last;
            end case;
         end if;
         if Lo > Hi or else Lo > Full_Hi or else Hi < Full_Lo then
            -- Null range or completely disjoint from Val_Pir_Ty representable values: always fails!
            -- Use Emit_Check_Fail_If(1) so CFG stays valid without unreachable zero-predecessor blocks.
            declare
               Always_Bad : constant String := Next_Reg (Ctx.E, "rfail");
            begin
               Emit_Inst (Ctx.E, Always_Bad & " = const.i8 1");
               Emit_Check_Fail_If (Ctx, Always_Bad);
            end;
            return;
         end if;
         if Lo <= Full_Lo and then Hi >= Full_Hi then
            return;
         end if;
         declare
            Need_Lo : constant Boolean := Lo > Full_Lo;
            Need_Hi : constant Boolean := Hi < Full_Hi;
            C1, C2, Bad : Unbounded_String;
         begin
            if Need_Lo and then Need_Hi then
               C1  := To_Unbounded_String (Next_Reg (Ctx.E, "rlo"));
               C2  := To_Unbounded_String (Next_Reg (Ctx.E, "rhi"));
               Bad := To_Unbounded_String (Next_Reg (Ctx.E, "rbad"));
               Emit_Inst (Ctx.E, To_String (C1) & " = " & Lt_Op & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Lo)));
               Emit_Inst (Ctx.E, To_String (C2) & " = " & Gt_Op & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Hi)));
               Emit_Inst (Ctx.E, To_String (Bad) & " = or.i8 " & To_String (C1) & ", " & To_String (C2));
               Emit_Check_Fail_If (Ctx, To_String (Bad));
            elsif Need_Lo then
               C1 := To_Unbounded_String (Next_Reg (Ctx.E, "rlo"));
               Emit_Inst (Ctx.E, To_String (C1) & " = " & Lt_Op & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Lo)));
               Emit_Check_Fail_If (Ctx, To_String (C1));
            elsif Need_Hi then
               C2 := To_Unbounded_String (Next_Reg (Ctx.E, "rhi"));
               Emit_Inst (Ctx.E, To_String (C2) & " = " & Gt_Op & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Hi)));
               Emit_Check_Fail_If (Ctx, To_String (C2));
            end if;
         end;
      end;
   end Emit_Range_Check;

   procedure Get_Array_Dim_Bounds
     (Ctx       : in out Context;
      Arr       : Expr_Val;
      Dim       : Natural;
      First_Reg : out Unbounded_String;
      Last_Reg  : out Unbounded_String)
   is
   begin
      if Arr.Kind = EK_Slice and then Dim >= 1 and then Dim <= Arr.NDims then
         First_Reg := Arr.First_Regs (Dim);
         Last_Reg  := Arr.Last_Regs (Dim);
         return;
      end if;
      declare
         TI : constant Type_Info := Get_Type_Info (Ctx, Arr.Ada_Type);
      begin
         if Dim >= 1 and then Dim <= Arr.NDims and then Length (Arr.First_Regs (Dim)) > 0 then
            First_Reg := Arr.First_Regs (Dim);
            Last_Reg  := Arr.Last_Regs (Dim);
            return;
         elsif TI.Kind = TK_Constrained_Array and then Dim >= 1 and then Dim <= TI.NDims and then TI.Dims (Dim).Is_Static then
            First_Reg := To_Unbounded_String (Emit_Int_Const (Ctx, TI.Dims (Dim).First, "i64", "af"));
            Last_Reg  := To_Unbounded_String (Emit_Int_Const (Ctx, TI.Dims (Dim).Last, "i64", "al"));
            return;
         elsif TI.Kind = TK_Constrained_Array and then Dim >= 1 and then Dim <= TI.NDims
           and then not Is_Void_Tree (TI.Dims (Dim).Idx_Type)
           and then not Expr_Has_Unbound_Discr (Ctx, TI.Dims (Dim).Idx_Type)
         then
            Emit_Index_Bounds (Ctx, TI.Dims (Dim).Idx_Type, First_Reg, Last_Reg);
            return;
         end if;
      end;
      declare
         RF : constant String := Next_Reg (Ctx.E, "af");
         RL : constant String := Next_Reg (Ctx.E, "al");
      begin
         Emit_Inst (Ctx.E, RF & " = const.i64 1");
         Emit_Inst (Ctx.E, RL & " = const.i64 0");
         First_Reg := To_Unbounded_String (RF);
         Last_Reg  := To_Unbounded_String (RL);
      end;
   end Get_Array_Dim_Bounds;

   function Resolve_Array_View (Ctx : in out Context; V : Expr_Val) return Array_View is
      TI  : constant Type_Info := Get_Type_Info (Ctx, V.Ada_Type);
      ETI : constant Type_Info := Get_Type_Info (Ctx, TI.Elem_Type);
      ND  : constant Natural := Natural'Max (1, Natural'Min ((if V.NDims > 0 then V.NDims else TI.NDims), Max_Array_Dims));
      Res : Array_View;
   begin
      Res.Data_Ptr   := V.Reg;
      Res.Ada_Type   := V.Ada_Type;
      Res.Elem_Type  := TI.Elem_Type;
      Res.Elem_Size  := Elem_Storage_Size (ETI);
      Res.Elem_Align := Elem_Storage_Align (ETI);
      Res.NDims      := ND;
      for D_I in 1 .. ND loop
         Get_Array_Dim_Bounds (Ctx, V, D_I, Res.Firsts (D_I), Res.Lasts (D_I));
      end loop;
      return Res;
   end Resolve_Array_View;

   procedure Emit_Discrete_Range
     (Ctx      : in out Context;
      Rng_Node : TREE;
      Pir_Ty   : out Unbounded_String;
      Low_Reg  : out Unbounded_String;
      High_Reg : out Unbounded_String)
   is
      K : constant NODE_NAME := KIND (Rng_Node);
   begin
      if K = DN_RANGE then
         declare
            T   : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Rng_Node));
            BT  : constant TREE := Subst_Base_Type (Ctx, T);
            TI  : constant Type_Info := Get_Type_Info (Ctx, T);
            V1  : constant TREE := Safe_SM_Value (D (AS_EXP1, Rng_Node));
            V2  : constant TREE := Safe_SM_Value (D (AS_EXP2, Rng_Node));
            StB : Boolean := False;
            BLo, BHi : Integer_64 := 0;
         begin
            if Ctx.Enable_Checks and then not Is_Void_Tree (BT) and then Is_Discrete_Type (BT) then
               Get_Discrete_Bounds (BT, StB, BLo, BHi);
               if StB then
                  for VN in 1 .. 2 loop
                     declare
                        SV  : constant TREE := (if VN = 1 then V1 else V2);
                        Val : Integer_64;
                        Ovf : Boolean;
                     begin
                        if Has_Static_Val (SV) then
                           Eval_Int64_Checked (SV, Val, Ovf);
                           if Ovf or else Val < BLo or else Val > BHi then
                              Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                              declare
                                 Unr : constant String := Next_Label (Ctx.E, "dro_u");
                              begin
                                 Emit_Label (Ctx.E, Unr);
                              end;
                           end if;
                        end if;
                     end;
                  end loop;
               end if;
            end if;
            declare
               E1 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng_Node), T);
               E2 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng_Node), T);
            begin
               Pir_Ty := TI.Pir_Type;
               if Is_Fixed_Type (T) and then (To_String (E1.Pir_Type) = "f32" or else To_String (E1.Pir_Type) = "f64") then
                  Low_Reg := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), T, To_String (Pir_Ty)));
               else
                  Low_Reg := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), To_String (Pir_Ty),
                               Is_Unsigned => Is_Unsigned_Scalar (E1.Ada_Type, To_String (E1.Pir_Type))));
               end if;
               if Is_Fixed_Type (T) and then (To_String (E2.Pir_Type) = "f32" or else To_String (E2.Pir_Type) = "f64") then
                  High_Reg := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), T, To_String (Pir_Ty)));
               else
                  High_Reg := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), To_String (Pir_Ty),
                                Is_Unsigned => Is_Unsigned_Scalar (E2.Ada_Type, To_String (E2.Pir_Type))));
               end if;
            end;
         end;
      elsif K = DN_DISCRETE_SUBTYPE then
         declare
            SI : constant TREE := D (AS_SUBTYPE_INDICATION, Rng_Node);
         begin
            if KIND (SI) = DN_SUBTYPE_INDICATION then
               declare
                  C : constant TREE := D (AS_CONSTRAINT, SI);
               begin
                  if not Is_Void_Tree (C) and then KIND (C) in DN_RANGE | DN_RANGE_ATTRIBUTE then
                     Emit_Discrete_Range (Ctx, C, Pir_Ty, Low_Reg, High_Reg);
                     if Ctx.Enable_Checks then
                        declare
                           Def    : constant TREE := Get_Name_Defn (D (AS_NAME, SI));
                           Mark_T : constant TREE :=
                             (if not Is_Void_Tree (Def) and then KIND (Def) in CLASS_TYPE_NAME
                              then Subst_Resolve_Type (Ctx, D (SM_TYPE_SPEC, Def)) else CONST_VOID);
                        begin
                           if not Is_Void_Tree (Mark_T) and then Is_Discrete_Type (Mark_T) then
                              declare
                                 NE    : constant String := Next_Reg (Ctx.E, "dsne");
                                 L_C   : constant String := Next_Label (Ctx.E, "dsc");
                                 L_O   : constant String := Next_Label (Ctx.E, "dso");
                                 Le_Op : constant String :=
                                   (if Is_Unsigned_Scalar (Mark_T, To_String (Pir_Ty)) then "cmp_le_u." else "cmp_le_s.");
                              begin
                                 Emit_Inst (Ctx.E, NE & " = " & Le_Op & To_String (Pir_Ty) & " " & To_String (Low_Reg) & ", " & To_String (High_Reg));
                                 Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                                 Emit_Label (Ctx.E, L_C);
                                 Emit_Range_Check (Ctx, To_String (Low_Reg), To_String (Pir_Ty), Mark_T);
                                 Emit_Range_Check (Ctx, To_String (High_Reg), To_String (Pir_Ty), Mark_T);
                                 Emit_Term (Ctx.E, "jmp " & L_O);
                                 Emit_Label (Ctx.E, L_O);
                              end;
                           end if;
                        end;
                     end if;
                     return;
                  end if;
               end;
               declare
                  Def : constant TREE := Get_Name_Defn (D (AS_NAME, SI));
                  TS  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def));
                  RTS : constant TREE := Resolve_Type_Spec (TS);
                  TI  : constant Type_Info := Get_Type_Info (Ctx, TS);
                  St  : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Pir_Ty := TI.Pir_Type;
                  if not Is_Void_Tree (RTS) and then Ctx.Saved_Type_Ranges.Contains (Tree_Key (RTS)) then
                     declare
                        SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (RTS));
                     begin
                        if SR.Is_Global or else SR.Owner_Subp = Ctx.Cur_Subp_Key then
                           Low_Reg  := To_Unbounded_String
                             (Emit_Scalar_Coerce (Ctx, Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global),
                                                  To_String (SR.Pir_Type), To_String (Pir_Ty)));
                           High_Reg := To_Unbounded_String
                             (Emit_Scalar_Coerce (Ctx, Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global),
                                                  To_String (SR.Pir_Type), To_String (Pir_Ty)));
                           return;
                        end if;
                     end;
                  end if;
                  Get_Discrete_Bounds (TS, St, Lo, Hi);
                  if St then
                     declare
                        RL : constant String := Next_Reg (Ctx.E, "lo");
                        RH : constant String := Next_Reg (Ctx.E, "hi");
                     begin
                        Emit_Inst (Ctx.E, RL & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Lo)));
                        Emit_Inst (Ctx.E, RH & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Hi)));
                        Low_Reg  := To_Unbounded_String (RL);
                        High_Reg := To_Unbounded_String (RH);
                     end;
                  else
                     Emit_Discrete_Range (Ctx, D (SM_RANGE, RTS), Pir_Ty, Low_Reg, High_Reg);
                  end if;
               end;
            else
               declare
                  Def : constant TREE := Get_Name_Defn (SI);
                  TS  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def));
                  RTS : constant TREE := Resolve_Type_Spec (TS);
                  TI  : constant Type_Info := Get_Type_Info (Ctx, TS);
                  St  : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Pir_Ty := TI.Pir_Type;
                  if not Is_Void_Tree (RTS) and then Ctx.Saved_Type_Ranges.Contains (Tree_Key (RTS)) then
                     declare
                        SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (RTS));
                     begin
                        if SR.Is_Global or else SR.Owner_Subp = Ctx.Cur_Subp_Key then
                           Low_Reg  := To_Unbounded_String
                             (Emit_Scalar_Coerce (Ctx, Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global),
                                                  To_String (SR.Pir_Type), To_String (Pir_Ty)));
                           High_Reg := To_Unbounded_String
                             (Emit_Scalar_Coerce (Ctx, Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global),
                                                  To_String (SR.Pir_Type), To_String (Pir_Ty)));
                           return;
                        end if;
                     end;
                  end if;
                  Get_Discrete_Bounds (TS, St, Lo, Hi);
                  if St then
                     declare
                        RL : constant String := Next_Reg (Ctx.E, "lo");
                        RH : constant String := Next_Reg (Ctx.E, "hi");
                     begin
                        Emit_Inst (Ctx.E, RL & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Lo)));
                        Emit_Inst (Ctx.E, RH & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Hi)));
                        Low_Reg  := To_Unbounded_String (RL);
                        High_Reg := To_Unbounded_String (RH);
                     end;
                  else
                     Emit_Discrete_Range (Ctx, D (SM_RANGE, RTS), Pir_Ty, Low_Reg, High_Reg);
                  end if;
               end;
            end if;
         end;
      elsif K = DN_RANGE_ATTRIBUTE then
         declare
            Pfx : constant TREE := D (AS_NAME, Rng_Node);
            Dim : Natural := 1;
            Exp : constant TREE := D (AS_EXP, Rng_Node);
         begin
            if not Is_Void_Tree (Exp) and then Has_Static_Val (Safe_SM_Value (Exp)) then
               Dim := Natural (Eval_Int64 (Safe_SM_Value (Exp)));
            end if;
            -- Prefix can be an array object or an array type mark (including Pkg.Arr_Type)
            declare
               Pfx_Def : constant TREE :=
                 (if KIND (Pfx) in DN_USED_NAME_ID | DN_SELECTED then Get_Name_Defn (Pfx) else CONST_VOID);
            begin
            if not Is_Void_Tree (Pfx_Def) and then KIND (Pfx_Def) in CLASS_TYPE_NAME then
               declare
                  TS : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Pfx_Def));
                  TI : constant Type_Info := Get_Type_Info (Ctx, TS);
                  IT : constant TREE := TI.Dims (Dim).Idx_Type;
                  ITI : constant Type_Info := Get_Type_Info (Ctx, IT);
                  RL : constant String := Next_Reg (Ctx.E, "lo");
                  RH : constant String := Next_Reg (Ctx.E, "hi");
               begin
                  Pir_Ty := ITI.Pir_Type;
                  if TI.Dims (Dim).Is_Static then
                     Emit_Inst (Ctx.E, RL & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (TI.Dims (Dim).First)));
                     Emit_Inst (Ctx.E, RH & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (TI.Dims (Dim).Last)));
                     Low_Reg  := To_Unbounded_String (RL);
                     High_Reg := To_Unbounded_String (RH);
                  else
                     Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (IT)), Pir_Ty, Low_Reg, High_Reg);
                  end if;
               end;
            else
               declare
                  Pfx_T : constant TREE := (if KIND (Pfx) in CLASS_EXP
                                            then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx)))
                                            else CONST_VOID);
                  Arr   : Expr_Val;
                  F32, L32 : Unbounded_String;
               begin
                  if not Is_Void_Tree (Pfx_T) and then Is_Access_Type (Pfx_T) then
                     declare
                        PV    : constant Expr_Val := Emit_Expr (Ctx, Pfx);
                        Des_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T)));
                     begin
                        Emit_Null_Check (Ctx, To_String (PV.Reg));
                        if Is_Access_To_Uncon_Array (Pfx_T) then
                           Emit_Unpack_Arr_Desc (Ctx, To_String (PV.Reg), Des_T, Arr);
                        else
                           Arr.Kind := EK_Addr; Arr.Reg := PV.Reg; Arr.Ada_Type := Des_T;
                        end if;
                     end;
                  else
                     Arr := Emit_LValue (Ctx, Pfx);
                  end if;
                  declare
                     TI  : constant Type_Info := Get_Type_Info (Ctx, Arr.Ada_Type);
                     IT  : constant TREE := TI.Dims (Dim).Idx_Type;
                     ITI : constant Type_Info := Get_Type_Info (Ctx, IT);
                  begin
                     Get_Array_Dim_Bounds (Ctx, Arr, Dim, F32, L32);
                     Pir_Ty := ITI.Pir_Type;
                     Low_Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F32), "i64", To_String (Pir_Ty)));
                     High_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L32), "i64", To_String (Pir_Ty)));
                  end;
               end;
            end if;
            end;
         end;
      else
         Pir_Ty   := To_Unbounded_String ("i32");
         Low_Reg  := To_Unbounded_String ("0");
         High_Reg := To_Unbounded_String ("0");
      end if;
   end Emit_Discrete_Range;

   procedure Emit_Index_Bounds
     (Ctx      : in out Context;
      Idx_Type : TREE;
      First_R  : out Unbounded_String;
      Last_R   : out Unbounded_String)
   is
      IT     : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Idx_Type));
      St     : Boolean := False;
      Lo, Hi : Integer_64 := 1;
   begin
      if not Is_Void_Tree (IT) and then Ctx.Saved_Type_Ranges.Contains (Tree_Key (IT)) then
         declare
            SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (IT));
         begin
            if SR.Is_Global or else SR.Owner_Subp = Ctx.Cur_Subp_Key then
               declare
                  LR0  : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                  HR0  : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                  Is_U : constant Boolean := Is_Unsigned_Scalar (IT, To_String (SR.Pir_Type));
               begin
                  First_R := To_Unbounded_String (Emit_Int_Convert (Ctx, LR0, To_String (SR.Pir_Type), "i64", Is_Unsigned => Is_U));
                  Last_R  := To_Unbounded_String (Emit_Int_Convert (Ctx, HR0, To_String (SR.Pir_Type), "i64", Is_Unsigned => Is_U));
                  return;
               end;
            end if;
         end;
      end if;
      if not Is_Void_Tree (IT) then
         Get_Discrete_Bounds (IT, St, Lo, Hi);
      end if;
      if St then
         First_R := To_Unbounded_String (Emit_Int_Const (Ctx, Lo, "i64", "ibf"));
         Last_R  := To_Unbounded_String (Emit_Int_Const (Ctx, Hi, "i64", "ibl"));
      elsif not Is_Void_Tree (IT) and then not Is_Void_Tree (D (SM_RANGE, IT)) then
         declare
            RT, RL, RH : Unbounded_String;
            Is_U : Boolean;
         begin
            Emit_Discrete_Range (Ctx, D (SM_RANGE, IT), RT, RL, RH);
            Is_U := Is_Unsigned_Scalar (IT, To_String (RT));
            First_R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64", Is_Unsigned => Is_U));
            Last_R  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64", Is_Unsigned => Is_U));
         end;
      else
         declare
            RF : constant String := Next_Reg (Ctx.E, "ibf");
            RL : constant String := Next_Reg (Ctx.E, "ibl");
         begin
            Emit_Inst (Ctx.E, RF & " = const.i64 1");
            Emit_Inst (Ctx.E, RL & " = const.i64 0");
            First_R := To_Unbounded_String (RF);
            Last_R  := To_Unbounded_String (RL);
         end;
      end if;
   end Emit_Index_Bounds;

   function Emit_Dim_Len_I64
     (Ctx      : in out Context;
      First_64 : String;
      Last_64  : String) return String
   is
      Ge  : constant String := Next_Reg (Ctx.E, "dlg");
      D64 : constant String := Next_Reg (Ctx.E, "dld");
      Sat : constant String := Next_Reg (Ctx.E, "dls");
      N64 : constant String := Next_Reg (Ctx.E, "dln");
      C64 : constant String := Next_Reg (Ctx.E, "dlc");
      Res : constant String := Next_Reg (Ctx.E, "dlr");
   begin
      Emit_Inst (Ctx.E, Ge  & " = cmp_ge_s.i64 " & Last_64 & ", " & First_64);
      Emit_Inst (Ctx.E, D64 & " = sub.i64 " & Last_64 & ", " & First_64);
      Emit_Inst (Ctx.E, Sat & " = cmp_ge_u.i64 " & D64 & ", 9223372036854775807");
      Emit_Inst (Ctx.E, N64 & " = add.i64 " & D64 & ", 1");
      Emit_Inst (Ctx.E, C64 & " = select.i64 " & Sat & ", 9223372036854775807, " & N64);
      Emit_Inst (Ctx.E, Res & " = select.i64 " & Ge & ", " & C64 & ", 0");
      return Res;
   end Emit_Dim_Len_I64;

   procedure Emit_Array_Byte_Size_I64
     (Ctx       : in out Context;
      Firsts    : Str_Array_4;
      Lasts     : Str_Array_4;
      NDims     : Natural;
      Elem_Sz   : Natural;
      Tot_Elems : out Unbounded_String;
      Tot_Bytes : out Unbounded_String)
   is
      ND    : constant Natural := Natural'Max (1, Natural'Min (NDims, Max_Array_Dims));
      ESz   : constant Natural := Natural'Max (1, Elem_Sz);
      Lens  : Str_Array_4;
      Any_Z : Unbounded_String;
   begin
      for D_I in 1 .. ND loop
         Lens (D_I) := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I))));
         if ND > 1 then
            declare
               IsZ : constant String := Next_Reg (Ctx.E, "asz");
            begin
               Emit_Inst (Ctx.E, IsZ & " = cmp_eq.i64 " & To_String (Lens (D_I)) & ", 0");
               if D_I = 1 then
                  Any_Z := To_Unbounded_String (IsZ);
               else
                  declare
                     OrZ : constant String := Next_Reg (Ctx.E, "aso");
                  begin
                     Emit_Inst (Ctx.E, OrZ & " = or.i8 " & To_String (Any_Z) & ", " & IsZ);
                     Any_Z := To_Unbounded_String (OrZ);
                  end;
               end if;
            end;
         end if;
      end loop;

      if ND = 1 then
         declare
            Byt : constant String := Next_Reg (Ctx.E, "asb");
            Ovf : constant String := Next_Reg (Ctx.E, "asov");
         begin
            Tot_Elems := Lens (1);
            Emit_Inst (Ctx.E, Byt & ", " & Ovf & " = mul_ovf_s.i64 " & To_String (Lens (1)) & ", " & Img (ESz));
            if Ctx.Enable_Checks then
               Emit_Check_Fail_If (Ctx, Ovf, Storage_Error_Tag (Ctx));
            end if;
            Tot_Bytes := To_Unbounded_String (Byt);
         end;
      else
         -- Super-null safe: if ANY dimension has length 0, total elements & bytes are 0 without overflowing!
         declare
            L_Nz   : constant String := Next_Label (Ctx.E, "asnz");
            L_Done : constant String := Next_Label (Ctx.E, "asdn");
            Cur    : Unbounded_String := Lens (1);
            Byt    : constant String := Next_Reg (Ctx.E, "asb");
            OvfB   : constant String := Next_Reg (Ctx.E, "asob");
            Out_T  : constant String := Next_Reg (Ctx.E, "ast");
            Out_B  : constant String := Next_Reg (Ctx.E, "asbt");
         begin
            Emit_Term (Ctx.E, "br " & To_String (Any_Z) & ", " & L_Done & "(0, 0), " & L_Nz);
            Emit_Label (Ctx.E, L_Nz);
            for D_I in 2 .. ND loop
               declare
                  M   : constant String := Next_Reg (Ctx.E, "asm");
                  Ovf : constant String := Next_Reg (Ctx.E, "asov");
               begin
                  Emit_Inst (Ctx.E, M & ", " & Ovf & " = mul_ovf_s.i64 " & To_String (Cur) & ", " & To_String (Lens (D_I)));
                  if Ctx.Enable_Checks then
                     Emit_Check_Fail_If (Ctx, Ovf, Storage_Error_Tag (Ctx));
                  end if;
                  Cur := To_Unbounded_String (M);
               end;
            end loop;
            Emit_Inst (Ctx.E, Byt & ", " & OvfB & " = mul_ovf_s.i64 " & To_String (Cur) & ", " & Img (ESz));
            if Ctx.Enable_Checks then
               Emit_Check_Fail_If (Ctx, OvfB, Storage_Error_Tag (Ctx));
            end if;
            Emit_Term (Ctx.E, "jmp " & L_Done & "(" & To_String (Cur) & ", " & Byt & ")");
            Emit_Label (Ctx.E, L_Done, Out_T & ": i64, " & Out_B & ": i64");
            Tot_Elems := To_Unbounded_String (Out_T);
            Tot_Bytes := To_Unbounded_String (Out_B);
         end;
      end if;
   end Emit_Array_Byte_Size_I64;

   procedure Emit_Array_Length_Check
     (Ctx   : in out Context;
      LHS   : Expr_Val;
      RHS   : Expr_Val;
      NDims : Natural)
   is
      ND : constant Natural := Natural'Max (1, Natural'Min (NDims, Max_Array_Dims));
      Any_Mis : Unbounded_String;
      L_Null  : Unbounded_String;
      R_Null  : Unbounded_String;
   begin
      if not Ctx.Enable_Checks then
         return;
      end if;
      for D_I in 1 .. ND loop
         declare
            LF, LL, RF, RL : Unbounded_String;
            C1, C2 : Unbounded_String;
            Mis : constant String := Next_Reg (Ctx.E, "almis");
            LZ  : constant String := Next_Reg (Ctx.E, "allz");
            RZ  : constant String := Next_Reg (Ctx.E, "alrz");
         begin
            Get_Array_Dim_Bounds (Ctx, LHS, D_I, LF, LL);
            Get_Array_Dim_Bounds (Ctx, RHS, D_I, RF, RL);
            C1 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (LF), To_String (LL)));
            C2 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (RF), To_String (RL)));
            Emit_Inst (Ctx.E, Mis & " = cmp_ne.i64 " & To_String (C1) & ", " & To_String (C2));
            Emit_Inst (Ctx.E, LZ  & " = cmp_eq.i64 " & To_String (C1) & ", 0");
            Emit_Inst (Ctx.E, RZ  & " = cmp_eq.i64 " & To_String (C2) & ", 0");
            if D_I = 1 then
               Any_Mis := To_Unbounded_String (Mis);
               L_Null  := To_Unbounded_String (LZ);
               R_Null  := To_Unbounded_String (RZ);
            else
               declare
                  M2 : constant String := Next_Reg (Ctx.E, "alm2");
                  L2 : constant String := Next_Reg (Ctx.E, "all2");
                  R2 : constant String := Next_Reg (Ctx.E, "alr2");
               begin
                  Emit_Inst (Ctx.E, M2 & " = or.i8 " & To_String (Any_Mis) & ", " & Mis);
                  Emit_Inst (Ctx.E, L2 & " = or.i8 " & To_String (L_Null) & ", " & LZ);
                  Emit_Inst (Ctx.E, R2 & " = or.i8 " & To_String (R_Null) & ", " & RZ);
                  Any_Mis := To_Unbounded_String (M2);
                  L_Null  := To_Unbounded_String (L2);
                  R_Null  := To_Unbounded_String (R2);
               end;
            end if;
         end;
      end loop;
      -- Ada 83 LRM 5.2.1(2): two null arrays of the same type always match lengths!
      declare
         Both_Null : constant String := Next_Reg (Ctx.E, "albn");
         Not_Both  : constant String := Next_Reg (Ctx.E, "alnb");
         Bad       : constant String := Next_Reg (Ctx.E, "albad");
      begin
         Emit_Inst (Ctx.E, Both_Null & " = and.i8 " & To_String (L_Null) & ", " & To_String (R_Null));
         Emit_Inst (Ctx.E, Not_Both  & " = xor.i8 " & Both_Null & ", 1");
         Emit_Inst (Ctx.E, Bad       & " = and.i8 " & To_String (Any_Mis) & ", " & Not_Both);
         Emit_Check_Fail_If (Ctx, Bad);
      end;
   end Emit_Array_Length_Check;

   -- Helper: true if T is an access type whose base designated type is an unconstrained array
   function Is_Access_To_Uncon_Array (T : TREE) return Boolean is
      R : constant TREE := Resolve_Type_Spec (T);
      B : constant TREE := Resolve_Type_Spec (Base_Type_Spec (T));
   begin
      if not Is_Void_Tree (B) and then (KIND (B) = DN_ACCESS or else KIND (B) = DN_CONSTRAINED_ACCESS) then
         declare
            DT : constant TREE := Resolve_Type_Spec (D (SM_DESIG_TYPE, B));
         begin
            if not Is_Void_Tree (DT) and then KIND (DT) = DN_ARRAY then
               return True;
            end if;
         end;
      end if;
      if not Is_Void_Tree (R) and then (KIND (R) = DN_ACCESS or else KIND (R) = DN_CONSTRAINED_ACCESS) then
         declare
            DT : constant TREE := Resolve_Type_Spec (D (SM_DESIG_TYPE, R));
         begin
            if not Is_Void_Tree (DT) and then KIND (DT) = DN_ARRAY then
               return True;
            end if;
         end;
      end if;
      return False;
   end Is_Access_To_Uncon_Array;

   procedure Emit_Store_Arr_Desc
     (Ctx      : in out Context;
      Desc_Ptr : String;
      Data_Ptr : String;
      Firsts   : Str_Array_4;
      Lasts    : Str_Array_4;
      ND       : Natural;
      Desc_Ty  : String := "@__ada_arr_desc")
   is
      P0 : constant String := Next_Reg (Ctx.E, "adp0");
   begin
      Emit_Inst (Ctx.E, P0 & " = field_ptr " & Desc_Ty & ", " & Desc_Ptr & ", 0");
      Emit_Inst (Ctx.E, "store.ptr " & Data_Ptr & ", " & P0 & ", align 8");
      for D_Idx in 1 .. Natural'Min (ND, Max_Array_Dims) loop
         declare
            FP : constant String := Next_Reg (Ctx.E, "adfp");
            LP : constant String := Next_Reg (Ctx.E, "adlp");
         begin
            Emit_Inst (Ctx.E, FP & " = field_ptr " & Desc_Ty & ", " & Desc_Ptr & ", " & Img (2 * D_Idx - 1));
            Emit_Inst (Ctx.E, "store.i64 " & To_String (Firsts (D_Idx)) & ", " & FP & ", align 8");
            Emit_Inst (Ctx.E, LP & " = field_ptr " & Desc_Ty & ", " & Desc_Ptr & ", " & Img (2 * D_Idx));
            Emit_Inst (Ctx.E, "store.i64 " & To_String (Lasts (D_Idx)) & ", " & LP & ", align 8");
         end;
      end loop;
   end Emit_Store_Arr_Desc;

   procedure Emit_Unpack_Arr_Desc
     (Ctx      : in out Context;
      Desc_Ptr : String;
      Des_T    : TREE;
      Out_Val  : out Expr_Val;
      Desc_Ty  : String := "@__ada_arr_desc")
   is
      DTI : constant Type_Info := Get_Type_Info (Ctx, Des_T);
      ETI : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
      ND  : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
      P0  : constant String := Next_Reg (Ctx.E, "adp");
      D0  : constant String := Next_Reg (Ctx.E, "add");
   begin
      Emit_Inst (Ctx.E, P0 & " = field_ptr " & Desc_Ty & ", " & Desc_Ptr & ", 0");
      Emit_Inst (Ctx.E, D0 & " = load.ptr " & P0 & ", align 8");
      Out_Val.Kind     := EK_Slice;
      Out_Val.Reg      := To_Unbounded_String (D0);
      Out_Val.Pir_Type := To_Unbounded_String ("ptr");
      Out_Val.Ada_Type := Des_T;
      Out_Val.Align    := ETI.Align;
      Out_Val.NDims    := ND;
      for D_Idx in 1 .. ND loop
         declare
            FP : constant String := Next_Reg (Ctx.E, "adfp");
            FV : constant String := Next_Reg (Ctx.E, "adfv");
            LP : constant String := Next_Reg (Ctx.E, "adlp");
            LV : constant String := Next_Reg (Ctx.E, "adlv");
         begin
            Emit_Inst (Ctx.E, FP & " = field_ptr " & Desc_Ty & ", " & Desc_Ptr & ", " & Img (2 * D_Idx - 1));
            Emit_Inst (Ctx.E, FV & " = load.i64 " & FP & ", align 8");
            Emit_Inst (Ctx.E, LP & " = field_ptr " & Desc_Ty & ", " & Desc_Ptr & ", " & Img (2 * D_Idx));
            Emit_Inst (Ctx.E, LV & " = load.i64 " & LP & ", align 8");
            Out_Val.First_Regs (D_Idx) := To_Unbounded_String (FV);
            Out_Val.Last_Regs (D_Idx)  := To_Unbounded_String (LV);
         end;
      end loop;
   end Emit_Unpack_Arr_Desc;

   procedure Emit_Exact_Bounds_Check
     (Ctx    : in out Context;
      F1, L1 : Str_Array_4;
      F2, L2 : Str_Array_4;
      NDims  : Natural)
   is
   begin
      if not Ctx.Enable_Checks then
         return;
      end if;
      for D_I in 1 .. Natural'Min (NDims, Max_Array_Dims) loop
         declare
            C1  : constant String := Next_Reg (Ctx.E, "eb1");
            C2  : constant String := Next_Reg (Ctx.E, "eb2");
            Bad : constant String := Next_Reg (Ctx.E, "ebb");
         begin
            Emit_Inst (Ctx.E, C1 & " = cmp_ne.i64 " & To_String (F1 (D_I)) & ", " & To_String (F2 (D_I)));
            Emit_Inst (Ctx.E, C2 & " = cmp_ne.i64 " & To_String (L1 (D_I)) & ", " & To_String (L2 (D_I)));
            Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
            Emit_Check_Fail_If (Ctx, Bad);
         end;
      end loop;
   end Emit_Exact_Bounds_Check;

   procedure Emit_Non_Null_Index_Check
     (Ctx      : in out Context;
      First_64 : String;
      Last_64  : String;
      Idx_Type : TREE)
   is
   begin
      if not Ctx.Enable_Checks or else Is_Void_Tree (Idx_Type) then
         return;
      end if;
      declare
         NE  : constant String := Next_Reg (Ctx.E, "nnne");
         L_C : constant String := Next_Label (Ctx.E, "nnc");
         L_O : constant String := Next_Label (Ctx.E, "nno");
      begin
         Emit_Inst (Ctx.E, NE & " = cmp_le_s.i64 " & First_64 & ", " & Last_64);
         Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
         Emit_Label (Ctx.E, L_C);
         Emit_Range_Check (Ctx, First_64, "i64", Idx_Type);
         Emit_Range_Check (Ctx, Last_64, "i64", Idx_Type);
         Emit_Term (Ctx.E, "jmp " & L_O);
         Emit_Label (Ctx.E, L_O);
      end;
   end Emit_Non_Null_Index_Check;

   -- Emit a null pointer check on an access value
   procedure Emit_Null_Check (Ctx : in out Context; Ptr_Reg : String) is
   begin
      if not Ctx.Enable_Checks then
         return;
      end if;
      declare
         Is_Null : constant String := Next_Reg (Ctx.E, "nchk");
      begin
         Emit_Inst (Ctx.E, Is_Null & " = cmp_eq.ptr " & Ptr_Reg & ", 0");
         Emit_Check_Fail_If (Ctx, Is_Null);
      end;
   end Emit_Null_Check;

   function Expr_Has_Unbound_Discr (Ctx : Context; Node : TREE; Depth : Natural := 0) return Boolean is
      K : NODE_NAME;
   begin
      if Is_Void_Tree (Node) or else Depth > 16 then
         return False;
      end if;
      K := KIND (Node);
      case K is
         when DN_USED_OBJECT_ID =>
            declare
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Node)));
            begin
               return not Is_Void_Tree (Def)
                 and then KIND (Def) = DN_DISCRIMINANT_ID
                 and then not Ctx.Vars.Contains (Tree_Key (Def));
            end;
         when DN_RANGE =>
            return Expr_Has_Unbound_Discr (Ctx, D (AS_EXP1, Node), Depth + 1)
              or else Expr_Has_Unbound_Discr (Ctx, D (AS_EXP2, Node), Depth + 1);
         when DN_INTEGER | DN_ENUMERATION =>
            return Expr_Has_Unbound_Discr (Ctx, D (SM_RANGE, Node), Depth + 1);
         when DN_SCALAR_S | DN_EXP_S =>
            declare
               L : SEQ_TYPE := LIST (Node);
               It : TREE;
            begin
               while not IS_EMPTY (L) loop
                  POP (L, It);
                  if Expr_Has_Unbound_Discr (Ctx, It, Depth + 1) then
                     return True;
                  end if;
               end loop;
               return False;
            end;
         when DN_PARENTHESIZED | DN_CONVERSION | DN_QUALIFIED =>
            return Expr_Has_Unbound_Discr (Ctx, D (AS_EXP, Node), Depth + 1);
         when DN_FUNCTION_CALL =>
            return Expr_Has_Unbound_Discr (Ctx, D (SM_NORMALIZED_PARAM_S, Node), Depth + 1);
         when others =>
            return False;
      end case;
   end Expr_Has_Unbound_Discr;

   procedure Emit_Discriminant_Check
     (Ctx        : in out Context;
      Rec_Ptr    : String;
      Target_Rec : TREE;
      Is_Access  : Boolean := False)
   is
      RT : TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Target_Rec));
      RT_Orig : constant TREE := RT;
   begin
      if not Ctx.Enable_Checks or else Is_Void_Tree (RT) then
         return;
      end if;
      if Is_Access then
         if KIND (RT) /= DN_ACCESS and then KIND (RT) /= DN_CONSTRAINED_ACCESS then
            return;
         end if;
         RT := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, RT)));
      end if;
      if Is_Void_Tree (RT) then
         return;
      end if;
      if Is_Access and then KIND (RT) = DN_CONSTRAINED_ARRAY
        and then Is_Access_To_Uncon_Array (RT_Orig)
      then
         -- LRM 3.8(4) / 4.6(11): constrained access-to-array exact bound check (skip if null)
         declare
            Sub_S  : constant TREE := D (SM_INDEX_SUBTYPE_S, RT);
            Is_N   : constant String := Next_Reg (Ctx.E, "acn");
            L_Do   : constant String := Next_Label (Ctx.E, "acd");
            L_Skip : constant String := Next_Label (Ctx.E, "acs");
            IL     : SEQ_TYPE := (if not Is_Void_Tree (Sub_S) then LIST (Sub_S) else MAKE);
            Idx_T  : TREE;
            Dim    : Natural := 0;
         begin
            if Is_Void_Tree (Sub_S) or else Expr_Has_Unbound_Discr (Ctx, Sub_S) then
               return;
            end if;
            Emit_Inst (Ctx.E, Is_N & " = cmp_eq.ptr " & Rec_Ptr & ", 0");
            Emit_Term (Ctx.E, "br " & Is_N & ", " & L_Skip & ", " & L_Do);
            Emit_Label (Ctx.E, L_Do);
            while not IS_EMPTY (IL) and then Dim < Max_Array_Dims loop
               POP (IL, Idx_T);
               Dim := Dim + 1;
               declare
                  FP  : constant String := Next_Reg (Ctx.E, "acfp");
                  FV  : constant String := Next_Reg (Ctx.E, "acfv");
                  LP  : constant String := Next_Reg (Ctx.E, "aclp");
                  LV  : constant String := Next_Reg (Ctx.E, "aclv");
                  TF, TL : Unbounded_String;
                  CF  : constant String := Next_Reg (Ctx.E, "accf");
                  CL  : constant String := Next_Reg (Ctx.E, "accl");
                  Bad : constant String := Next_Reg (Ctx.E, "acbd");
               begin
                  Emit_Inst (Ctx.E, FP & " = field_ptr @__ada_arr_desc, " & Rec_Ptr & ", " & Img (2 * Dim - 1));
                  Emit_Inst (Ctx.E, FV & " = load.i64 " & FP & ", align 8");
                  Emit_Inst (Ctx.E, LP & " = field_ptr @__ada_arr_desc, " & Rec_Ptr & ", " & Img (2 * Dim));
                  Emit_Inst (Ctx.E, LV & " = load.i64 " & LP & ", align 8");
                  Emit_Index_Bounds (Ctx, Idx_T, TF, TL);
                  Emit_Inst (Ctx.E, CF & " = cmp_ne.i64 " & FV & ", " & To_String (TF));
                  Emit_Inst (Ctx.E, CL & " = cmp_ne.i64 " & LV & ", " & To_String (TL));
                  Emit_Inst (Ctx.E, Bad & " = or.i8 " & CF & ", " & CL);
                  Emit_Check_Fail_If (Ctx, Bad);
               end;
            end loop;
            Emit_Term (Ctx.E, "jmp " & L_Skip);
            Emit_Label (Ctx.E, L_Skip);
         end;
         return;
      end if;
      if KIND (RT) /= DN_CONSTRAINED_RECORD then
         return;
      end if;
      declare
         BT  : constant TREE := Subst_Base_Type (Ctx, RT);
         DS  : constant TREE := (if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then D (SM_DISCRIMINANT_S, BT) else CONST_VOID);
         ND  : constant TREE := D (SM_NORMALIZED_DSCRMT_S, RT);
         RTI : constant Type_Info := Get_Type_Info (Ctx, BT);
         L_Skip : Unbounded_String;
      begin
         if Is_Void_Tree (DS) or else Is_Void_Tree (ND) or else Expr_Has_Unbound_Discr (Ctx, ND) then
            return;
         end if;
         if Is_Access then
            -- Null access value satisfies any discriminant constraint (LRM 4.6/3.8)
            declare
               Is_N : constant String := Next_Reg (Ctx.E, "dcn");
               L_Do : constant String := Next_Label (Ctx.E, "dcd");
            begin
               L_Skip := To_Unbounded_String (Next_Label (Ctx.E, "dcs"));
               Emit_Inst (Ctx.E, Is_N & " = cmp_eq.ptr " & Rec_Ptr & ", 0");
               Emit_Term (Ctx.E, "br " & Is_N & ", " & To_String (L_Skip) & ", " & L_Do);
               Emit_Label (Ctx.E, L_Do);
            end;
         end if;
         declare
            DL   : SEQ_TYPE := LIST (DS);
            NDL  : SEQ_TYPE := LIST (ND);
            DD   : TREE;
            FIdx : Natural := 0;
         begin
            while not IS_EMPTY (DL) and then not IS_EMPTY (NDL) loop
               POP (DL, DD);
               if KIND (DD) = DN_DSCRMT_DECL then
                  declare
                     NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                     DID : TREE;
                     Exp : TREE;
                  begin
                     while not IS_EMPTY (NS) and then not IS_EMPTY (NDL) loop
                        POP (NS, DID);
                        POP (NDL, Exp);
                        declare
                           CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                           CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                           FP  : constant String := Next_Reg (Ctx.E, "dcfp");
                           Act : constant String := Next_Reg (Ctx.E, "dcav");
                           ER  : Unbounded_String;
                           Neq : constant String := Next_Reg (Ctx.E, "dcne");
                        begin
                           if Ctx.Saved_Type_Discs.Contains (Tree_Key (RT))
                             and then (Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Is_Global
                                       or else Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Owner_Subp = Ctx.Cur_Subp_Key)
                             and then FIdx + 1 <= Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Count
                           then
                              declare
                                 SD : constant Saved_Disc_Info := Ctx.Saved_Type_Discs.Element (Tree_Key (RT));
                                 LV : constant String := Load_Saved_Slot (Ctx, To_String (SD.Ptrs (FIdx + 1)), To_String (SD.Pir_Types (FIdx + 1)), SD.Aligns (FIdx + 1), SD.Is_Global);
                              begin
                                 ER := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, LV, To_String (SD.Pir_Types (FIdx + 1)), To_String (CTI.Pir_Type)));
                              end;
                           else
                              declare
                                 EV : constant Expr_Val := Emit_Expr (Ctx, Exp, CT);
                              begin
                                 ER := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), To_String (CTI.Pir_Type)));
                              end;
                           end if;
                           declare
                              Can_D : constant TREE := Canonical_Def (DID);
                              DK    : constant Unsigned_32 := Tree_Key (Can_D);
                           begin
                              if Ctx.Fields.Contains (DK) then
                                 declare
                                    FE : constant Field_Entry := Ctx.Fields.Element (DK);
                                    FV : constant String := Emit_Load_Field_Scalar (Ctx, Rec_Ptr, To_String (RTI.Pir_Type), FE, CT, CTI, "dca");
                                 begin
                                    Emit_Inst (Ctx.E, Neq & " = cmp_ne." & To_String (CTI.Pir_Type) & " " & FV & ", " & To_String (ER));
                                 end;
                              else
                                 Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & Rec_Ptr & ", " & Img (FIdx));
                                 Emit_Inst (Ctx.E, Act & " = load." & To_String (CTI.Pir_Type) & " " & FP & ", align " & Img (CTI.Align));
                                 Emit_Inst (Ctx.E, Neq & " = cmp_ne." & To_String (CTI.Pir_Type) & " " & Act & ", " & To_String (ER));
                              end if;
                           end;
                           Emit_Check_Fail_If (Ctx, Neq);
                        end;
                        FIdx := FIdx + 1;
                     end loop;
                  end;
               end if;
            end loop;
         end;
         if Is_Access then
            Emit_Term (Ctx.E, "jmp " & To_String (L_Skip));
            Emit_Label (Ctx.E, To_String (L_Skip));
         end if;
      end;
   end Emit_Discriminant_Check;

   -- LRM 4.6(9) (c46043a, c46044a, c64103c/d): check that array component subtypes have matching constraints!
   procedure Emit_Subtypes_Match_Check
     (Ctx   : in out Context;
      Src_T : TREE;
      Dst_T : TREE;
      Depth : Natural := 0)
   is
      ST : constant TREE := Subst_Resolve_Type (Ctx, Src_T);
      DT : constant TREE := Subst_Resolve_Type (Ctx, Dst_T);
   begin
      if not Ctx.Enable_Checks or else Depth > 6
        or else Is_Void_Tree (ST) or else Is_Void_Tree (DT) or else ST = DT
      then
         return;
      end if;

      if KIND (ST) in DN_INTEGER | DN_ENUMERATION and then KIND (DT) in DN_INTEGER | DN_ENUMERATION then
         declare
            L1, H1, L2, H2 : Unbounded_String;
            T1, T2         : Unbounded_String;
            procedure Get_Sc_Bounds (T : TREE; PT, Lo, Hi : out Unbounded_String) is
               St : Boolean;
               LoI, HiI : Integer_64;
               TI : constant Type_Info := Get_Type_Info (Ctx, T);
            begin
               PT := TI.Pir_Type;
               Get_Discrete_Bounds (T, St, LoI, HiI);
               if St then
                  declare
                     RL : constant String := Next_Reg (Ctx.E, "sml");
                     RH : constant String := Next_Reg (Ctx.E, "smh");
                  begin
                     Emit_Inst (Ctx.E, RL & " = const." & To_String (PT) & " " & Img (Long_Long_Integer (LoI)));
                     Emit_Inst (Ctx.E, RH & " = const." & To_String (PT) & " " & Img (Long_Long_Integer (HiI)));
                     Lo := To_Unbounded_String (RL);
                     Hi := To_Unbounded_String (RH);
                  end;
               elsif Ctx.Saved_Type_Ranges.Contains (Tree_Key (T))
                 and then (Ctx.Saved_Type_Ranges.Element (Tree_Key (T)).Is_Global
                           or else Ctx.Saved_Type_Ranges.Element (Tree_Key (T)).Owner_Subp = Ctx.Cur_Subp_Key)
               then
                  declare
                     SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (T));
                  begin
                     PT := SR.Pir_Type;
                     Lo := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global));
                     Hi := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global));
                  end;
               else
                  Emit_Discrete_Range (Ctx, D (SM_RANGE, T), PT, Lo, Hi);
               end if;
            end Get_Sc_Bounds;
         begin
            Get_Sc_Bounds (ST, T1, L1, H1);
            Get_Sc_Bounds (DT, T2, L2, H2);
            declare
               U1 : constant Boolean := Is_Unsigned_Scalar (ST, To_String (T1));
               U2 : constant Boolean := Is_Unsigned_Scalar (DT, To_String (T2));
               A1 : constant String := Emit_Int_Convert (Ctx, To_String (L1), To_String (T1), "i64", Is_Unsigned => U1);
               B1 : constant String := Emit_Int_Convert (Ctx, To_String (H1), To_String (T1), "i64", Is_Unsigned => U1);
               A2 : constant String := Emit_Int_Convert (Ctx, To_String (L2), To_String (T2), "i64", Is_Unsigned => U2);
               B2 : constant String := Emit_Int_Convert (Ctx, To_String (H2), To_String (T2), "i64", Is_Unsigned => U2);
               N1 : constant String := Next_Reg (Ctx.E, "smn1");
               N2 : constant String := Next_Reg (Ctx.E, "smn2");
               BN : constant String := Next_Reg (Ctx.E, "smbn");
               E1 : constant String := Next_Reg (Ctx.E, "sme1");
               E2 : constant String := Next_Reg (Ctx.E, "sme2");
               BE : constant String := Next_Reg (Ctx.E, "smbe");
               Ok : constant String := Next_Reg (Ctx.E, "smok");
               Bd : constant String := Next_Reg (Ctx.E, "smbd");
            begin
               Emit_Inst (Ctx.E, N1 & " = cmp_gt_s.i64 " & A1 & ", " & B1);
               Emit_Inst (Ctx.E, N2 & " = cmp_gt_s.i64 " & A2 & ", " & B2);
               Emit_Inst (Ctx.E, BN & " = and.i8 " & N1 & ", " & N2);
               Emit_Inst (Ctx.E, E1 & " = cmp_eq.i64 " & A1 & ", " & A2);
               Emit_Inst (Ctx.E, E2 & " = cmp_eq.i64 " & B1 & ", " & B2);
               Emit_Inst (Ctx.E, BE & " = and.i8 " & E1 & ", " & E2);
               Emit_Inst (Ctx.E, Ok & " = or.i8 " & BN & ", " & BE);
               Emit_Inst (Ctx.E, Bd & " = xor.i8 " & Ok & ", 1");
               Emit_Check_Fail_If (Ctx, Bd);
            end;
         end;

      elsif KIND (ST) = DN_CONSTRAINED_ARRAY and then KIND (DT) = DN_CONSTRAINED_ARRAY then
         declare
            STI : constant Type_Info := Get_Type_Info (Ctx, ST);
            DTI : constant Type_Info := Get_Type_Info (Ctx, DT);
            ND  : constant Natural := Natural'Min (STI.NDims, DTI.NDims);
         begin
            for D_I in 1 .. ND loop
               declare
                  F1, L1, F2, L2 : Unbounded_String;
                  N1 : constant String := Next_Reg (Ctx.E, "san1");
                  N2 : constant String := Next_Reg (Ctx.E, "san2");
                  BN : constant String := Next_Reg (Ctx.E, "sabn");
                  E1 : constant String := Next_Reg (Ctx.E, "sae1");
                  E2 : constant String := Next_Reg (Ctx.E, "sae2");
                  BE : constant String := Next_Reg (Ctx.E, "sabe");
                  Ok : constant String := Next_Reg (Ctx.E, "saok");
                  Bd : constant String := Next_Reg (Ctx.E, "sabd");
               begin
                  Emit_Index_Bounds (Ctx, STI.Dims (D_I).Idx_Type, F1, L1);
                  Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, F2, L2);
                  Emit_Inst (Ctx.E, N1 & " = cmp_gt_s.i64 " & To_String (F1) & ", " & To_String (L1));
                  Emit_Inst (Ctx.E, N2 & " = cmp_gt_s.i64 " & To_String (F2) & ", " & To_String (L2));
                  Emit_Inst (Ctx.E, BN & " = and.i8 " & N1 & ", " & N2);
                  Emit_Inst (Ctx.E, E1 & " = cmp_eq.i64 " & To_String (F1) & ", " & To_String (F2));
                  Emit_Inst (Ctx.E, E2 & " = cmp_eq.i64 " & To_String (L1) & ", " & To_String (L2));
                  Emit_Inst (Ctx.E, BE & " = and.i8 " & E1 & ", " & E2);
                  Emit_Inst (Ctx.E, Ok & " = or.i8 " & BN & ", " & BE);
                  Emit_Inst (Ctx.E, Bd & " = xor.i8 " & Ok & ", 1");
                  Emit_Check_Fail_If (Ctx, Bd);
               end;
            end loop;
            Emit_Subtypes_Match_Check (Ctx, STI.Elem_Type, DTI.Elem_Type, Depth + 1);
         end;

      elsif KIND (ST) = DN_CONSTRAINED_RECORD and then KIND (DT) = DN_CONSTRAINED_RECORD then
         declare
            CS1 : constant TREE := D (SM_NORMALIZED_DSCRMT_S, ST);
            CS2 : constant TREE := D (SM_NORMALIZED_DSCRMT_S, DT);
         begin
            if not Is_Void_Tree (CS1) and then not Is_Void_Tree (CS2) then
               declare
                  L1 : SEQ_TYPE := LIST (CS1);
                  L2 : SEQ_TYPE := LIST (CS2);
                  E1, E2 : TREE;
                  D_I : Natural := 0;
               begin
                  while not IS_EMPTY (L1) and then not IS_EMPTY (L2) loop
                     POP (L1, E1);
                     POP (L2, E2);
                     D_I := D_I + 1;
                     declare
                        V1, V2 : Unbounded_String;
                        T1, T2 : Unbounded_String;
                        A1, A2 : Unbounded_String;
                        Ne     : constant String := Next_Reg (Ctx.E, "sdne");
                     begin
                        if Ctx.Saved_Type_Discs.Contains (Tree_Key (ST))
                          and then D_I <= Ctx.Saved_Type_Discs.Element (Tree_Key (ST)).Count
                          and then (Ctx.Saved_Type_Discs.Element (Tree_Key (ST)).Is_Global
                                    or else Ctx.Saved_Type_Discs.Element (Tree_Key (ST)).Owner_Subp = Ctx.Cur_Subp_Key)
                        then
                           declare
                              SD : constant Saved_Disc_Info := Ctx.Saved_Type_Discs.Element (Tree_Key (ST));
                           begin
                              T1 := SD.Pir_Types (D_I);
                              V1 := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SD.Ptrs (D_I)), To_String (T1), SD.Aligns (D_I), SD.Is_Global));
                           end;
                        else
                           declare
                              EV : constant Expr_Val := Emit_Expr (Ctx, E1);
                           begin
                              T1 := EV.Pir_Type; V1 := EV.Reg;
                           end;
                        end if;
                        if Ctx.Saved_Type_Discs.Contains (Tree_Key (DT))
                          and then D_I <= Ctx.Saved_Type_Discs.Element (Tree_Key (DT)).Count
                          and then (Ctx.Saved_Type_Discs.Element (Tree_Key (DT)).Is_Global
                                    or else Ctx.Saved_Type_Discs.Element (Tree_Key (DT)).Owner_Subp = Ctx.Cur_Subp_Key)
                        then
                           declare
                              SD : constant Saved_Disc_Info := Ctx.Saved_Type_Discs.Element (Tree_Key (DT));
                           begin
                              T2 := SD.Pir_Types (D_I);
                              V2 := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SD.Ptrs (D_I)), To_String (T2), SD.Aligns (D_I), SD.Is_Global));
                           end;
                        else
                           declare
                              EV : constant Expr_Val := Emit_Expr (Ctx, E2);
                           begin
                              T2 := EV.Pir_Type; V2 := EV.Reg;
                           end;
                        end if;
                        A1 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V1), To_String (T1), "i64"));
                        A2 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V2), To_String (T2), "i64"));
                        Emit_Inst (Ctx.E, Ne & " = cmp_ne.i64 " & To_String (A1) & ", " & To_String (A2));
                        Emit_Check_Fail_If (Ctx, Ne);
                     end;
                  end loop;
               end;
            end if;
         end;

      elsif (KIND (ST) = DN_CONSTRAINED_RECORD and then KIND (DT) /= DN_CONSTRAINED_RECORD)
        or else (KIND (DT) = DN_CONSTRAINED_RECORD and then KIND (ST) /= DN_CONSTRAINED_RECORD)
        or else (KIND (ST) = DN_CONSTRAINED_ACCESS and then KIND (DT) /= DN_CONSTRAINED_ACCESS)
        or else (KIND (DT) = DN_CONSTRAINED_ACCESS and then KIND (ST) /= DN_CONSTRAINED_ACCESS)
      then
         Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
         declare
            Unr : constant String := Next_Label (Ctx.E, "smu");
         begin
            Emit_Label (Ctx.E, Unr);
         end;

      elsif KIND (ST) = DN_CONSTRAINED_ACCESS and then KIND (DT) = DN_CONSTRAINED_ACCESS then
         Emit_Subtypes_Match_Check (Ctx, D (SM_DESIG_TYPE, ST), D (SM_DESIG_TYPE, DT), Depth + 1);

      elsif KIND (ST) in DN_FLOAT | DN_FIXED and then KIND (DT) in DN_FLOAT | DN_FIXED then
         -- Compare static bounds and accuracy (c46043a/44a)
         declare
            R1 : constant TREE := D (SM_RANGE, ST);
            R2 : constant TREE := D (SM_RANGE, DT);
         begin
            if KIND (ST) = DN_FLOAT and then KIND (DT) = DN_FLOAT
              and then not Is_Void_Tree (D (SM_ACCURACY, ST)) and then not Is_Void_Tree (D (SM_ACCURACY, DT))
              and then Has_Static_Val (D (SM_ACCURACY, ST)) and then Has_Static_Val (D (SM_ACCURACY, DT))
              and then Eval_Int64 (D (SM_ACCURACY, ST)) /= Eval_Int64 (D (SM_ACCURACY, DT))
            then
               Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
               declare
                  Unr : constant String := Next_Label (Ctx.E, "smfu");
               begin
                  Emit_Label (Ctx.E, Unr);
               end;
               return;
            elsif KIND (ST) = DN_FIXED and then KIND (DT) = DN_FIXED
              and then Fixed_Delta (ST) > 0.0 and then Fixed_Delta (DT) > 0.0
              and then Fixed_Delta (ST) /= Fixed_Delta (DT)
            then
               Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
               declare
                  Unr : constant String := Next_Label (Ctx.E, "smxu");
               begin
                  Emit_Label (Ctx.E, Unr);
               end;
               return;
            end if;
            if not Is_Void_Tree (R1) and then KIND (R1) = DN_RANGE
              and then not Is_Void_Tree (R2) and then KIND (R2) = DN_RANGE
              and then R1 /= R2
            then
               declare
                  V1L : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, R1), ST);
                  V1H : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, R1), ST);
                  V2L : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, R2), DT);
                  V2H : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, R2), DT);
               begin
                  if KIND (ST) = DN_FLOAT then
                     declare
                        A1 : constant String := Emit_Scalar_Coerce (Ctx, To_String (V1L.Reg), To_String (V1L.Pir_Type), "f64");
                        B1 : constant String := Emit_Scalar_Coerce (Ctx, To_String (V1H.Reg), To_String (V1H.Pir_Type), "f64");
                        A2 : constant String := Emit_Scalar_Coerce (Ctx, To_String (V2L.Reg), To_String (V2L.Pir_Type), "f64");
                        B2 : constant String := Emit_Scalar_Coerce (Ctx, To_String (V2H.Reg), To_String (V2H.Pir_Type), "f64");
                        N1 : constant String := Next_Reg (Ctx.E, "sfn1");
                        N2 : constant String := Next_Reg (Ctx.E, "sfn2");
                        BN : constant String := Next_Reg (Ctx.E, "sfbn");
                        E1 : constant String := Next_Reg (Ctx.E, "sfe1");
                        E2 : constant String := Next_Reg (Ctx.E, "sfe2");
                        BE : constant String := Next_Reg (Ctx.E, "sfbe");
                        Ok : constant String := Next_Reg (Ctx.E, "sfok");
                        Bd : constant String := Next_Reg (Ctx.E, "sfbd");
                     begin
                        Emit_Inst (Ctx.E, N1 & " = fcmp_ogt.f64 " & A1 & ", " & B1);
                        Emit_Inst (Ctx.E, N2 & " = fcmp_ogt.f64 " & A2 & ", " & B2);
                        Emit_Inst (Ctx.E, BN & " = and.i8 " & N1 & ", " & N2);
                        Emit_Inst (Ctx.E, E1 & " = fcmp_oeq.f64 " & A1 & ", " & A2);
                        Emit_Inst (Ctx.E, E2 & " = fcmp_oeq.f64 " & B1 & ", " & B2);
                        Emit_Inst (Ctx.E, BE & " = and.i8 " & E1 & ", " & E2);
                        Emit_Inst (Ctx.E, Ok & " = or.i8 " & BN & ", " & BE);
                        Emit_Inst (Ctx.E, Bd & " = xor.i8 " & Ok & ", 1");
                        Emit_Check_Fail_If (Ctx, Bd);
                     end;
                  else
                     declare
                        function To_Fixed_I64 (EV : Expr_Val; FT_Ref : TREE) return String is
                           PT : constant String := To_String (EV.Pir_Type);
                        begin
                           if PT = "f32" or else PT = "f64" then
                              return Emit_F64_To_Fixed_Ticks (Ctx, To_String (EV.Reg), PT, FT_Ref, "i64");
                           else
                              return Emit_Int_Convert (Ctx, To_String (EV.Reg), PT, "i64",
                                Is_Unsigned => Is_Unsigned_Scalar (EV.Ada_Type, PT));
                           end if;
                        end To_Fixed_I64;
                        A1 : constant String := To_Fixed_I64 (V1L, R1);
                        B1 : constant String := To_Fixed_I64 (V1H, R1);
                        A2 : constant String := To_Fixed_I64 (V2L, R2);
                        B2 : constant String := To_Fixed_I64 (V2H, R2);
                        N1 : constant String := Next_Reg (Ctx.E, "sxn1");
                        N2 : constant String := Next_Reg (Ctx.E, "sxn2");
                        BN : constant String := Next_Reg (Ctx.E, "sxbn");
                        E1 : constant String := Next_Reg (Ctx.E, "sxe1");
                        E2 : constant String := Next_Reg (Ctx.E, "sxe2");
                        BE : constant String := Next_Reg (Ctx.E, "sxbe");
                        Ok : constant String := Next_Reg (Ctx.E, "sxok");
                        Bd : constant String := Next_Reg (Ctx.E, "sxbd");
                     begin
                        Emit_Inst (Ctx.E, N1 & " = cmp_gt_s.i64 " & A1 & ", " & B1);
                        Emit_Inst (Ctx.E, N2 & " = cmp_gt_s.i64 " & A2 & ", " & B2);
                        Emit_Inst (Ctx.E, BN & " = and.i8 " & N1 & ", " & N2);
                        Emit_Inst (Ctx.E, E1 & " = cmp_eq.i64 " & A1 & ", " & A2);
                        Emit_Inst (Ctx.E, E2 & " = cmp_eq.i64 " & B1 & ", " & B2);
                        Emit_Inst (Ctx.E, BE & " = and.i8 " & E1 & ", " & E2);
                        Emit_Inst (Ctx.E, Ok & " = or.i8 " & BN & ", " & BE);
                        Emit_Inst (Ctx.E, Bd & " = xor.i8 " & Ok & ", 1");
                        Emit_Check_Fail_If (Ctx, Bd);
                     end;
                  end if;
               end;
            end if;
         end;
      end if;
   end Emit_Subtypes_Match_Check;

   function Emit_LValue (Ctx : in out Context; Node : TREE) return Expr_Val is
      Res : Expr_Val;
      K   : constant NODE_NAME := KIND (Node);
   begin
      case K is
         when DN_USED_OBJECT_ID | DN_USED_NAME_ID =>
            declare
               Raw_Def : constant TREE := D (SM_DEFN, Node);
               Def     : constant TREE := Canonical_Def (Subst_Tree (Ctx, Raw_Def));
               Key     : constant Unsigned_32 := Tree_Key (Def);
            begin
               if Ctx.Vars.Contains (Key) then
                  declare
                     V  : constant Var_Info := Ctx.Vars.Element (Key);
                     ATy : constant TREE := Subst_Tree (Ctx, V.Ada_Type);
                     TI : constant Type_Info := Get_Type_Info (Ctx, ATy);
                  begin
                     Res.Ada_Type := ATy;
                     Res.Align    := V.Align;
                     Res.Pir_Type := TI.Pir_Type;
                     if V.Kind = VK_Uncon_Array_Param or else V.Kind = VK_Dyn_Array then
                        Res.Kind  := EK_Slice;
                        Res.NDims := V.NDims;
                        if V.Has_Desc and then
                          (Length (V.Desc_Global) > 0
                           or else (V.Owner_Subp /= 0 and then V.Owner_Subp /= Ctx.Cur_Subp_Key))
                        then
                           declare
                              DP : Unbounded_String;
                           begin
                              if Length (V.Desc_Global) > 0 then
                                 declare
                                    GA : constant String := Next_Reg (Ctx.E, "dg");
                                 begin
                                    Emit_Inst (Ctx.E, GA & " = addr " & To_String (V.Desc_Global));
                                    DP := To_Unbounded_String (GA);
                                 end;
                              else
                                 DP := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                              end if;
                              Emit_Unpack_Arr_Desc (Ctx, To_String (DP), V.Ada_Type, Res);
                              Res.Align := V.Align;
                              Res.NDims := V.NDims;
                           end;
                        else
                           Res.Reg        := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                           Res.First_Regs := V.First_Regs;
                           Res.Last_Regs  := V.Last_Regs;
                        end if;
                     elsif V.Kind = VK_Param_Val then
                        -- Shouldn't be modified, but return value
                        Res.Kind := EK_Scalar;
                        Res.Reg  := V.Pir_Name;
                     elsif V.Kind = VK_Const_Int then
                        declare
                           CR : constant String := Next_Reg (Ctx.E, "lci");
                        begin
                           Emit_Inst (Ctx.E, CR & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (V.Const_I64)));
                           Res.Kind := EK_Scalar;
                           Res.Reg  := To_Unbounded_String (CR);
                        end;
                     elsif V.Kind = VK_Const_Float then
                        declare
                           CR : constant String := Next_Reg (Ctx.E, "lcf");
                        begin
                           Emit_Inst (Ctx.E, CR & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (V.Const_F64));
                           Res.Kind := EK_Scalar;
                           Res.Reg  := To_Unbounded_String (CR);
                        end;
                     else
                        Res.Kind := EK_Addr;
                        Res.Reg  := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                     end if;
                     return Res;
                  end;
               else
                  -- External package variable from a with'd unit (never treat unbound discriminant as external global!)
                  if KIND (Def) in CLASS_TYPE_NAME | DN_TASK_BODY_ID then
                     declare
                        TTS : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def)));
                     begin
                        if Is_Task_Type (TTS) then
                           Res.Kind := EK_Scalar;
                           Res.Reg  := To_Unbounded_String (Emit_Enclosing_Task_Self_By_Key (Ctx, Tree_Key (TTS)));
                           Res.Pir_Type := To_Unbounded_String ("ptr");
                           Res.Ada_Type := TTS; Res.Align := 8;
                           return Res;
                        end if;
                     end;
                  end if;
                  if KIND (Def) not in CLASS_OBJECT_NAME or else KIND (Def) = DN_DISCRIMINANT_ID then
                     declare
                        ATy : constant TREE := (if KIND (Def) = DN_DISCRIMINANT_ID then Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def)) else CONST_VOID);
                        TI  : constant Type_Info := Get_Type_Info (Ctx, ATy);
                        R   : constant String := Next_Reg (Ctx.E, "nobj");
                     begin
                        if KIND (Def) = DN_DISCRIMINANT_ID and then TI.Kind = TK_Scalar then
                           Emit_Entry_Alloca (Ctx.E, R & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                           Emit_Inst (Ctx.E, "store." & To_String (TI.Pir_Type) & " 0, " & R & ", align " & Img (TI.Align));
                           Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ATy; Res.Align := TI.Align;
                        else
                           Emit_Inst (Ctx.E, R & " = const.ptr 0");
                           Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := To_Unbounded_String ("ptr");
                           Res.Ada_Type := CONST_VOID; Res.Align := 8;
                        end if;
                        return Res;
                     end;
                  end if;
                  declare
                     ATy      : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def));
                     TI       : constant Type_Info := Get_Type_Info (Ctx, ATy);
                     QName    : constant String := Qualified_Ada_Name (Def);
                     Is_Ren   : constant Boolean :=
                       KIND (Def) in DN_VARIABLE_ID | DN_CONSTANT_ID and then DB (SM_RENAMES_OBJ, Def);
                     Is_DynA  : constant Boolean :=
                       TI.Kind = TK_Unconstrained_Array or else (TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic);
                     Is_SubpP : constant Boolean := KIND (Def) in DN_IN_ID | DN_OUT_ID | DN_IN_OUT_ID;
                  begin
                     if Is_Ren then
                        declare
                           Ren_E    : constant TREE := D (SM_INIT_EXP, Def);
                           Is_Slice : constant Boolean :=
                             Is_DynA or else (not Is_Void_Tree (Ren_E) and then KIND (Ren_E) = DN_SLICE);
                        begin
                           if Is_Slice then
                              declare
                                 GSym : constant String := "@ada_rnd__" & QName;
                                 AR   : constant String := Next_Reg (Ctx.E, "erda");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global @__ada_arr_desc, align 8");
                                 Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                                 Emit_Unpack_Arr_Desc (Ctx, AR, ATy, Res);
                                 Res.Align := TI.Align;
                                 return Res;
                              end;
                           else
                              declare
                                 GSym : constant String := "@ada_rnp__" & QName;
                                 AR   : constant String := Next_Reg (Ctx.E, "erpa");
                                 LP   : constant String := Next_Reg (Ctx.E, "erpp");
                              begin
                                 Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global ptr, align 8");
                                 Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, LP & " = load.ptr " & AR & ", align 8");
                                 Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (LP);
                                 Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ATy; Res.Align := TI.Align;
                                 return Res;
                              end;
                           end if;
                        end;
                     elsif Is_DynA then
                        -- External library dynamic/unconstrained array or subunit parent unconstrained-array param
                        declare
                           GSym : constant String := "@ada_g__" & QName;
                           AR   : constant String := Next_Reg (Ctx.E, "egda");
                        begin
                           Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global @__ada_arr_desc, align 8");
                           Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                           Emit_Unpack_Arr_Desc (Ctx, AR, ATy, Res);
                           Res.Align := TI.Align;
                           return Res;
                        end;
                     elsif Is_SubpP and then (TI.Kind = TK_Record or else TI.Kind = TK_Constrained_Array) then
                        -- Subunit accessing parent subprogram's composite formal parameter (exported ptr display)
                        declare
                           GSym : constant String := "@ada_g__" & QName;
                           AR   : constant String := Next_Reg (Ctx.E, "egpa");
                           LP   : constant String := Next_Reg (Ctx.E, "egpp");
                        begin
                           Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global ptr, align 8");
                           Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                           Emit_Inst (Ctx.E, LP & " = load.ptr " & AR & ", align 8");
                           Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (LP);
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ATy; Res.Align := TI.Align;
                           return Res;
                        end;
                     elsif KIND (Def) in DN_VARIABLE_ID | DN_CONSTANT_ID
                       and then not Is_Void_Tree (D (SM_ADDRESS, Def))
                     then
                        -- External library object with explicit `for X use at Addr;` clause
                        declare
                           GSym : constant String := "@ada_aptr__" & QName;
                           AR   : constant String := Next_Reg (Ctx.E, "egaa");
                           LP   : constant String := Next_Reg (Ctx.E, "egap");
                        begin
                           Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global ptr, align 8");
                           Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                           Emit_Inst (Ctx.E, LP & " = load.ptr " & AR & ", align 8");
                           Res.Kind     := EK_Addr;
                           Res.Reg      := To_Unbounded_String (LP);
                           Res.Pir_Type := TI.Pir_Type;
                           Res.Ada_Type := ATy;
                           Res.Align    := TI.Align;
                           return Res;
                        end;
                     else
                        declare
                           GSym : constant String := "@ada_g__" & QName;
                           AR   : constant String := Next_Reg (Ctx.E, "ega");
                        begin
                           Add_Global_Decl (Ctx.E, GSym,
                             "extern " & GSym & " = global " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                           Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                           Res.Kind     := EK_Addr;
                           Res.Reg      := To_Unbounded_String (AR);
                           Res.Pir_Type := TI.Pir_Type;
                           Res.Ada_Type := ATy;
                           Res.Align    := TI.Align;
                           return Res;
                        end;
                     end if;
                  end;
               end if;
            end;

         when DN_SELECTED =>
            declare
               Pfx   : constant TREE := D (AS_NAME, Node);
               Des   : TREE := D (AS_DESIGNATOR, Node);
               Def   : TREE;
            begin
               while not Is_Void_Tree (Des) and then KIND (Des) = DN_SELECTED loop
                  Des := D (AS_DESIGNATOR, Des);
               end loop;
               Def := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Des)));
               -- Package.Object?
               if KIND (Def) /= DN_COMPONENT_ID and then KIND (Def) /= DN_DISCRIMINANT_ID then
                  return Emit_LValue (Ctx, Des);
               end if;
               -- Record component access (with implicit access dereference if Pfx is access type!)
               declare
                  Base_LV : Expr_Val := Emit_LValue (Ctx, Pfx);
                  Pfx_T   : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Base_LV.Ada_Type));
                  Rec_Ptr : Unbounded_String := Base_LV.Reg;
                  Rec_T   : TREE := Pfx_T;
               begin
                  if Is_Access_Type (Pfx_T) then
                     if Base_LV.Kind = EK_Addr then
                        declare
                           LP : constant String := Next_Reg (Ctx.E, "ap");
                        begin
                           if Is_Shared_Object (Ctx, Pfx) then
                              Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8 [volatile]");
                              Emit_Inst (Ctx.E, "fence [seq_cst]");
                           else
                              Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8");
                           end if;
                           Rec_Ptr := To_Unbounded_String (LP);
                        end;
                     end if;
                     Emit_Null_Check (Ctx, To_String (Rec_Ptr));
                     Rec_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T)));
                  end if;
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, Rec_T);
                     CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def));
                     CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                     CK  : constant Unsigned_32 := Tree_Key (Def);
                     FP  : constant String := Next_Reg (Ctx.E, "fp");
                  begin
                     declare
                        Use_CK : Unsigned_32 := CK;
                     begin
                        if not Ctx.Fields.Contains (Use_CK) then
                           -- LRM 3.4 (c34009d/f/j/l): derived private/record type has copied discriminant IDs; match by name on base record
                           declare
                              Base_R : constant TREE := Subst_Base_Type (Ctx, Rec_T);
                              Want   : constant String := Sym_Name (Def);
                              procedure Find_In_List (List_Node : TREE) is
                                 L : SEQ_TYPE;
                                 It : TREE;
                              begin
                                 if Is_Void_Tree (List_Node) or else Ctx.Fields.Contains (Use_CK) then return; end if;
                                 L := LIST (List_Node);
                                 while not IS_EMPTY (L) and then not Ctx.Fields.Contains (Use_CK) loop
                                    POP (L, It);
                                    if KIND (It) in DN_DSCRMT_DECL | DN_VARIABLE_DECL then
                                       declare
                                          NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, It));
                                          ID : TREE;
                                       begin
                                          while not IS_EMPTY (NS) loop
                                             POP (NS, ID);
                                             if Sym_Name (ID) = Want and then Ctx.Fields.Contains (Tree_Key (ID)) then
                                                Use_CK := Tree_Key (ID);
                                                return;
                                             end if;
                                          end loop;
                                       end;
                                    end if;
                                 end loop;
                              end Find_In_List;
                              procedure Find_In_Comp_List (CL : TREE) is
                              begin
                                 if Is_Void_Tree (CL) or else Ctx.Fields.Contains (Use_CK) then return; end if;
                                 Find_In_List (D (AS_DECL_S, CL));
                                 if not Ctx.Fields.Contains (Use_CK) then
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
                                             while not IS_EMPTY (VL) and then not Ctx.Fields.Contains (Use_CK) loop
                                                POP (VL, VN);
                                                if KIND (VN) = DN_VARIANT then
                                                   Find_In_Comp_List (D (AS_COMP_LIST, VN));
                                                end if;
                                             end loop;
                                          end;
                                       end if;
                                    end;
                                 end if;
                              end Find_In_Comp_List;
                           begin
                              if not Is_Void_Tree (Base_R) and then KIND (Base_R) = DN_RECORD then
                                 Find_In_List (D (SM_DISCRIMINANT_S, Base_R));
                                 if not Ctx.Fields.Contains (Use_CK) and then not Is_Void_Tree (D (SM_COMP_LIST, Base_R)) then
                                    Find_In_Comp_List (D (SM_COMP_LIST, Base_R));
                                 end if;
                              end if;
                           end;
                        end if;
                     if Ctx.Fields.Contains (Use_CK) then
                        declare
                           FE : constant Field_Entry := Ctx.Fields.Element (Use_CK);
                        begin
                           -- LRM 4.1.3(16) (c41304b): check active discriminant matches variant containing Def!
                           if FE.In_Union and then Ctx.Enable_Checks then
                              Emit_Variant_Check_For_Field (Ctx, To_String (Rec_Ptr), Rec_T, Def, Use_CK);
                           end if;
                           declare
                              Addr_R : constant String := Emit_Field_Addr (Ctx, To_String (Rec_Ptr), To_String (RTI.Pir_Type), FE, "fp");
                           begin
                              Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Addr_R & ", 0");
                           end;
                        end;
                     else
                        Emit_Inst (Ctx.E,
                          FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " &
                          To_String (Rec_Ptr) & ", 0");
                     end if;
                     if CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                        -- Embedded @__ada_arr_desc inside record: unpack into EK_Slice!
                        Emit_Unpack_Arr_Desc (Ctx, FP, CT, Res);
                        return Res;
                     end if;
                     declare
                        Out_Align : Natural := CTI.Align;
                     begin
                        if Ctx.Fields.Contains (Use_CK) then
                           Out_Align := Explicit_Field_Align (Ctx.Fields.Element (Use_CK), CTI.Align);
                        end if;
                        Res.Kind     := EK_Addr;
                        Res.Reg      := To_Unbounded_String (FP);
                        Res.Pir_Type := CTI.Pir_Type;
                        Res.Ada_Type := CT;
                        Res.Align    := Out_Align;
                        return Res;
                     end;
                     end;
                  end;
               end;
            end;

         when DN_ALL =>
            declare
               Pfx   : constant Expr_Val := Emit_Expr (Ctx, D (AS_NAME, Node));
               Pfx_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Pfx.Ada_Type));
               Des_T : constant TREE :=
                 (if not Is_Void_Tree (Pfx_T) and then (KIND (Pfx_T) = DN_ACCESS or else KIND (Pfx_T) = DN_CONSTRAINED_ACCESS)
                  then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T)))
                  else CONST_VOID);
               DTI   : constant Type_Info := Get_Type_Info (Ctx, Des_T);
            begin
               Emit_Null_Check (Ctx, To_String (Pfx.Reg));
               if Is_Access_To_Uncon_Array (Pfx_T) then
                  Emit_Unpack_Arr_Desc (Ctx, To_String (Pfx.Reg), Des_T, Res);
                  return Res;
               end if;
               Res.Kind     := EK_Addr;
               Res.Reg      := Pfx.Reg;
               Res.Pir_Type := DTI.Pir_Type;
               Res.Ada_Type := Des_T;
               Res.Align    := DTI.Align;
               return Res;
            end;

         when DN_INDEXED =>
            declare
               Pfx_Node : constant TREE := D (AS_NAME, Node);
               Arr_Val  : Expr_Val;
               Arr_Ptr  : Unbounded_String;
               Arr_T    : TREE;
            begin
               if KIND (Pfx_Node) = DN_FUNCTION_CALL
                 or else KIND (Pfx_Node) = DN_NULLARY_CALL
                 or else KIND (Pfx_Node) = DN_SLICE
               then
                  Arr_Val := Emit_Expr (Ctx, Pfx_Node);
               else
                  Arr_Val := Emit_LValue (Ctx, Pfx_Node);
               end if;
               Arr_Ptr := Arr_Val.Reg;
               Arr_T   := Resolve_Type_Spec (Subst_Tree (Ctx, Arr_Val.Ada_Type));
               if Is_Access_Type (Arr_T) then
                  declare
                     Orig_Acc_T : constant TREE := Arr_T;
                  begin
                     if Arr_Val.Kind = EK_Addr then
                        declare
                           LP : constant String := Next_Reg (Ctx.E, "ap");
                        begin
                           if Is_Shared_Object (Ctx, Pfx_Node) then
                              Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Arr_Ptr) & ", align 8 [volatile]");
                              Emit_Inst (Ctx.E, "fence [seq_cst]");
                           else
                              Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Arr_Ptr) & ", align 8");
                           end if;
                           Arr_Ptr := To_Unbounded_String (LP);
                        end;
                     end if;
                     Emit_Null_Check (Ctx, To_String (Arr_Ptr));
                     Arr_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Orig_Acc_T)));
                     if Is_Access_To_Uncon_Array (Orig_Acc_T) then
                        Emit_Unpack_Arr_Desc (Ctx, To_String (Arr_Ptr), Arr_T, Arr_Val);
                        Arr_Ptr := Arr_Val.Reg;
                     else
                        Arr_Val.Ada_Type := Arr_T;
                     end if;
                  end;
               end if;

               declare
                  ATI     : constant Type_Info := Get_Type_Info (Ctx, Arr_T);
                  ETI     : constant Type_Info := Get_Type_Info (Ctx, ATI.Elem_Type);
                  Exps    : SEQ_TYPE := LIST (D (AS_EXP_S, Node));
                  Exp_N   : TREE;
                  Dim     : Natural := 0;
                  Cur_Ptr : Unbounded_String := Arr_Ptr;
                  Flat    : Unbounded_String;
               begin
                  while not IS_EMPTY (Exps) loop
                     POP (Exps, Exp_N);
                     Dim := Dim + 1;
                     declare
                        IV    : constant Expr_Val := Emit_Expr (Ctx, Exp_N);
                        IV_U  : constant Boolean := Is_Unsigned_Scalar (IV.Ada_Type, To_String (IV.Pir_Type));
                        I64   : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i64", Is_Unsigned => IV_U);
                        F_Reg : Unbounded_String;
                        L_Reg : Unbounded_String;
                        Off64 : constant String := Next_Reg (Ctx.E, "ioff64");
                     begin
                        Get_Array_Dim_Bounds (Ctx, Arr_Val, Dim, F_Reg, L_Reg);
                        declare
                           Skip_Chk : Boolean := not Ctx.Enable_Checks;
                        begin
                           if not Skip_Chk and then Arr_Val.Kind /= EK_Slice
                             and then Dim <= ATI.NDims and then ATI.Dims (Dim).Is_Static
                           then
                              if Has_Static_Val (Safe_SM_Value (Exp_N)) then
                                 declare
                                    SV : constant Integer_64 := Eval_Int64 (Safe_SM_Value (Exp_N));
                                 begin
                                    if SV >= ATI.Dims (Dim).First and then SV <= ATI.Dims (Dim).Last then
                                       Skip_Chk := True;
                                    end if;
                                 end;
                              elsif KIND (Exp_N) = DN_USED_OBJECT_ID then
                                 declare
                                    DK : constant Unsigned_32 := Tree_Key (Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Exp_N))));
                                 begin
                                    if Ctx.Vars.Contains (DK) then
                                       declare
                                          IVar : constant Var_Info := Ctx.Vars.Element (DK);
                                       begin
                                          if IVar.Has_Static_Rng
                                            and then IVar.Rng_Lo >= ATI.Dims (Dim).First
                                            and then IVar.Rng_Hi <= ATI.Dims (Dim).Last
                                          then
                                             Skip_Chk := True;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                           end if;
                           if not Skip_Chk then
                              declare
                                 C1  : constant String := Next_Reg (Ctx.E, "ilo");
                                 C2  : constant String := Next_Reg (Ctx.E, "ihi");
                                 Bad : constant String := Next_Reg (Ctx.E, "ibad");
                              begin
                                 Emit_Inst (Ctx.E, C1 & " = cmp_lt_s.i64 " & I64 & ", " & To_String (F_Reg));
                                 Emit_Inst (Ctx.E, C2 & " = cmp_gt_s.i64 " & I64 & ", " & To_String (L_Reg));
                                 Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                 Emit_Check_Fail_If (Ctx, Bad);
                              end;
                           end if;
                           Emit_Inst (Ctx.E, Off64 & " = sub.i64 " & I64 & ", " & To_String (F_Reg));
                        end;
                        if Dim < ATI.NDims then
                           -- Advance base pointer by row stride (enables LICM hoisting out of inner dim loops!)
                           declare
                              All_St : Boolean := True;
                              Prod   : Integer_64 := 1;
                           begin
                              for Sub_D in Dim + 1 .. ATI.NDims loop
                                 if ATI.Dims (Sub_D).Is_Static then
                                    if ATI.Dims (Sub_D).Last >= ATI.Dims (Sub_D).First then
                                       Prod := Prod * (ATI.Dims (Sub_D).Last - ATI.Dims (Sub_D).First + 1);
                                    else
                                       Prod := 0;
                                    end if;
                                 else
                                    All_St := False;
                                 end if;
                              end loop;
                              if All_St then
                                 declare
                                    R_Off : constant String := Next_Reg (Ctx.E, "roff");
                                 begin
                                    Emit_Inst (Ctx.E, R_Off & " = mul.i64 " & Off64 & ", " & Img (Long_Long_Integer (Prod)));
                                    Cur_Ptr := To_Unbounded_String (Emit_Elem_Ptr (Ctx, ETI, To_String (Cur_Ptr), R_Off));
                                 end;
                              else
                                 declare
                                    S_Acc : Unbounded_String := To_Unbounded_String (Off64);
                                 begin
                                    for Sub_D in Dim + 1 .. ATI.NDims loop
                                       declare
                                          SF, SL : Unbounded_String;
                                          L6 : Unbounded_String;
                                          Ml : constant String := Next_Reg (Ctx.E, "fmul");
                                       begin
                                          Get_Array_Dim_Bounds (Ctx, Arr_Val, Sub_D, SF, SL);
                                          L6 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (SF), To_String (SL)));
                                          Emit_Inst (Ctx.E, Ml & " = mul.i64 " & To_String (S_Acc) & ", " & To_String (L6));
                                          S_Acc := To_Unbounded_String (Ml);
                                       end;
                                    end loop;
                                    Cur_Ptr := To_Unbounded_String (Emit_Elem_Ptr (Ctx, ETI, To_String (Cur_Ptr), To_String (S_Acc)));
                                 end;
                              end if;
                           end;
                        else
                           Flat := To_Unbounded_String (Off64);
                        end if;
                     end;
                  end loop;

                  declare
                     EP : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (Cur_Ptr), To_String (Flat));
                  begin
                     if ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic then
                        Emit_Unpack_Arr_Desc (Ctx, EP, ATI.Elem_Type, Res);
                        return Res;
                     end if;
                     Res.Kind     := EK_Addr;
                     Res.Reg      := To_Unbounded_String (EP);
                     Res.Pir_Type := ETI.Pir_Type;
                     Res.Ada_Type := ATI.Elem_Type;
                     Res.Align    := ETI.Align;
                     return Res;
                  end;
               end;
            end;

         when DN_SLICE =>
            declare
               Pfx_Node : constant TREE := D (AS_NAME, Node);
               Pfx_Val  : Expr_Val;
               Arr_Ptr  : Unbounded_String;
               Arr_T    : TREE;
               Rng_T, Lo_R, Hi_R : Unbounded_String;
               AF_R, AL_R : Unbounded_String;
            begin
               if KIND (Pfx_Node) = DN_FUNCTION_CALL
                 or else KIND (Pfx_Node) = DN_NULLARY_CALL
                 or else KIND (Pfx_Node) = DN_SLICE
               then
                  Pfx_Val := Emit_Expr (Ctx, Pfx_Node);
               else
                  Pfx_Val := Emit_LValue (Ctx, Pfx_Node);
               end if;
               Arr_Ptr := Pfx_Val.Reg;
               Arr_T   := Resolve_Type_Spec (Subst_Tree (Ctx, Pfx_Val.Ada_Type));
               if Is_Access_Type (Arr_T) then
                  declare
                     Orig_Acc_T : constant TREE := Arr_T;
                  begin
                     if Pfx_Val.Kind = EK_Addr then
                        declare
                           LP : constant String := Next_Reg (Ctx.E, "sap");
                        begin
                           Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Arr_Ptr) & ", align 8");
                           Arr_Ptr := To_Unbounded_String (LP);
                        end;
                     end if;
                     Emit_Null_Check (Ctx, To_String (Arr_Ptr));
                     Arr_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Orig_Acc_T)));
                     if Is_Access_To_Uncon_Array (Orig_Acc_T) then
                        Emit_Unpack_Arr_Desc (Ctx, To_String (Arr_Ptr), Arr_T, Pfx_Val);
                        Arr_Ptr := Pfx_Val.Reg;
                     else
                        Pfx_Val.Reg      := Arr_Ptr;
                        Pfx_Val.Ada_Type := Arr_T;
                     end if;
                  end;
               end if;
               declare
                  ATI : constant Type_Info := Get_Type_Info (Ctx, Arr_T);
                  ETI : constant Type_Info := Get_Type_Info (Ctx, ATI.Elem_Type);
               begin
               Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Node), Rng_T, Lo_R, Hi_R);
               declare
                  Uns  : constant Boolean :=
                    ATI.NDims >= 1 and then not Is_Void_Tree (ATI.Dims (1).Idx_Type)
                    and then Is_Unsigned_Scalar (ATI.Dims (1).Idx_Type, To_String (Rng_T));
                  Lo64 : constant String := Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (Rng_T), "i64", Is_Unsigned => Uns);
                  Hi64 : constant String := Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (Rng_T), "i64", Is_Unsigned => Uns);
               begin
                  Get_Array_Dim_Bounds (Ctx, Pfx_Val, 1, AF_R, AL_R);

                  if Ctx.Enable_Checks then
                     -- Non-empty slice check in 64-bit signed space
                     declare
                        Non_Empty : constant String := Next_Reg (Ctx.E, "sne");
                        C1        : constant String := Next_Reg (Ctx.E, "slo");
                        C2        : constant String := Next_Reg (Ctx.E, "shi");
                        Oob       : constant String := Next_Reg (Ctx.E, "soob");
                        Bad       : constant String := Next_Reg (Ctx.E, "sbad");
                     begin
                        Emit_Inst (Ctx.E, Non_Empty & " = cmp_le_s.i64 " & Lo64 & ", " & Hi64);
                        Emit_Inst (Ctx.E, C1 & " = cmp_lt_s.i64 " & Lo64 & ", " & To_String (AF_R));
                        Emit_Inst (Ctx.E, C2 & " = cmp_gt_s.i64 " & Hi64 & ", " & To_String (AL_R));
                        Emit_Inst (Ctx.E, Oob & " = or.i8 " & C1 & ", " & C2);
                        Emit_Inst (Ctx.E, Bad & " = and.i8 " & Non_Empty & ", " & Oob);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end if;

                  declare
                     Off64 : constant String := Next_Reg (Ctx.E, "soff64");
                  begin
                     Emit_Inst (Ctx.E, Off64 & " = sub.i64 " & Lo64 & ", " & To_String (AF_R));
                     declare
                        SP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (Pfx_Val.Reg), Off64);
                     begin
                        Res.Kind           := EK_Slice;
                        Res.Reg            := To_Unbounded_String (SP2);
                        Res.Pir_Type       := To_Unbounded_String ("ptr");
                        Res.Ada_Type       := Base_Type_Spec (Arr_T);
                        Res.Align          := ETI.Align;
                        Res.NDims          := 1;
                        Res.First_Regs (1) := To_Unbounded_String (Lo64);
                        Res.Last_Regs (1)  := To_Unbounded_String (Hi64);
                        return Res;
                     end;
                  end;
               end;
               end;
            end;

         when DN_PARENTHESIZED =>
            return Emit_LValue (Ctx, D (AS_EXP, Node));

         when DN_CONVERSION =>
            -- View conversion on composite/array lvalue (LRM 4.6 / 6.4.1)
            declare
               ET  : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               ETI : constant Type_Info := Get_Type_Info (Ctx, ET);
            begin
               -- Avoid double-evaluating AS_EXP for non-composite conversions!
               if ETI.Kind not in TK_Constrained_Array | TK_Unconstrained_Array | TK_Record then
                  return Emit_Expr (Ctx, Node);
               end if;
               declare
                  Inner : constant Expr_Val := Emit_LValue (Ctx, D (AS_EXP, Node));
               begin
               if ETI.Kind = TK_Constrained_Array or else ETI.Kind = TK_Unconstrained_Array then
                  declare
                     STI_C : constant Type_Info := Get_Type_Info (Ctx, Inner.Ada_Type);
                     ND : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                     SF, SL : Str_Array_4;
                  begin
                     Emit_Subtypes_Match_Check (Ctx, STI_C.Elem_Type, ETI.Elem_Type);
                     for D_I in 1 .. ND loop
                        Get_Array_Dim_Bounds (Ctx, Inner, D_I, SF (D_I), SL (D_I));
                     end loop;
                     Res := Inner;
                     Res.Kind     := EK_Slice;
                     Res.Ada_Type := ET;
                     Res.NDims    := ND;
                     if ETI.Kind = TK_Constrained_Array then
                        declare
                           Tgt : Expr_Val;
                        begin
                           Tgt.Kind := EK_Slice; Tgt.Ada_Type := ET; Tgt.NDims := ND;
                           for D_I in 1 .. ND loop
                              Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                           end loop;
                           Emit_Array_Length_Check (Ctx, Tgt, Inner, ND);
                           Res.First_Regs := Tgt.First_Regs;
                           Res.Last_Regs  := Tgt.Last_Regs;
                        end;
                     else
                        if Ctx.Enable_Checks then
                           -- LRM 4.6(10) / AI-00313 (c64103c): check each dimension independently;
                           -- bounds always belong to target index base type, and for every non-null dimension
                           -- (SF <= SL) bounds must belong to target index subtype!
                           for D_I in 1 .. ND loop
                              declare
                                 IT  : constant TREE := ETI.Dims (D_I).Idx_Type;
                                 BIT : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, IT));
                              begin
                                 if not Is_Void_Tree (BIT) then
                                    Emit_Range_Check (Ctx, To_String (SF (D_I)), "i64", BIT);
                                    Emit_Range_Check (Ctx, To_String (SL (D_I)), "i64", BIT);
                                 end if;
                                 Emit_Non_Null_Index_Check (Ctx, To_String (SF (D_I)), To_String (SL (D_I)), IT);
                              end;
                           end loop;
                        end if;
                        Res.First_Regs := SF;
                        Res.Last_Regs  := SL;
                     end if;
                     return Res;
                  end;
               else
                  -- P3-6.7: Change of representation across derived record types must materialize converted buffer!
                  declare
                     Src_BR : constant TREE := Subst_Base_Type (Ctx, Inner.Ada_Type);
                     Dst_BR : constant TREE := Subst_Base_Type (Ctx, ET);
                     STI_R  : constant Type_Info := Get_Type_Info (Ctx, Src_BR);
                     DTI_R  : constant Type_Info := Get_Type_Info (Ctx, Dst_BR);
                  begin
                     if not Is_Void_Tree (Src_BR) and then not Is_Void_Tree (Dst_BR)
                       and then KIND (Src_BR) = DN_RECORD and then KIND (Dst_BR) = DN_RECORD
                       and then Src_BR /= Dst_BR
                       and then (not Is_Void_Tree (D (SM_REPRESENTATION, Src_BR))
                                 or else not Is_Void_Tree (D (SM_REPRESENTATION, Dst_BR))
                                 or else STI_R.Byte_Size /= DTI_R.Byte_Size)
                     then
                        declare
                           Buf : constant String := Next_Reg (Ctx.E, "rconv");
                           Sz  : constant Natural := Natural'Max (1, DTI_R.Byte_Size);
                           Al  : constant Natural := Natural'Max (1, DTI_R.Align);
                        begin
                           Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Sz) & " x i8], align " & Img (Al));
                           Emit_Record_Rep_Copy_Into (Ctx, Buf, ET, To_String (Inner.Reg), Inner.Ada_Type);
                           Emit_Discriminant_Check (Ctx, Buf, ET, Is_Access => False);
                           Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                           Res.Pir_Type := DTI_R.Pir_Type; Res.Ada_Type := ET; Res.Align := Al;
                           return Res;
                        end;
                     end if;
                  end;
                  Emit_Discriminant_Check (Ctx, To_String (Inner.Reg), ET, Is_Access => False);
                  Res := Inner;
                  Res.Ada_Type := ET;
                  return Res;
               end if;
               end;
            end;

         when others =>
            return Emit_Expr (Ctx, Node);
      end case;
   end Emit_LValue;

   function Is_Bitfield (FE : Field_Entry) return Boolean is
   begin
      return FE.Has_Explicit_Offset
        and then FE.Bit_Size > 0
        and then FE.Bit_Size <= 64
        and then (FE.Bit_Offset > 0 or else FE.Bit_Size /= FE.Field_Size * 8);
   end Is_Bitfield;

   function Emit_Create_Task_Handle
     (Ctx          : in out Context;
      Task_Spec    : TREE;
      Activate_Now : Boolean := False) return String
   is
      TS : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Task_Spec));
      TK : constant Unsigned_32 := Tree_Key (TS);
      TI : Task_Info;
      M_Reg  : Unbounded_String := Ctx.Cur_Master_Reg;
      Fn_Ptr : constant String := Next_Reg (Ctx.E, "tfn");
      Env_R  : Unbounded_String := To_Unbounded_String ("0");
      TH_Reg : Unbounded_String;
   begin
      Register_Task_Spec (Ctx, TS, Ctx.Cur_Subp_Key);
      if not Ctx.Tasks.Contains (TK) then
         declare
            Z : constant String := Next_Reg (Ctx.E, "t0");
         begin
            Emit_Inst (Ctx.E, Z & " = const.ptr 0");
            return Z;
         end;
      end if;
      TI := Ctx.Tasks.Element (TK);
      if Length (M_Reg) = 0 then
         Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", M_Reg, Nounwind => True);
      end if;
      if TI.Parent_Subp /= 0 then
         if TI.Parent_Subp = Ctx.Cur_Subp_Key then
            if Length (Ctx.Cur_Frame_Reg) > 0 then
               Env_R := Ctx.Cur_Frame_Reg;
            end if;
         elsif Length (Ctx.Cur_SL_Reg) > 0 then
            declare
               Cur_K : Unsigned_32 := Ctx.Cur_Subp_Key;
               SL    : Unbounded_String := Ctx.Cur_SL_Reg;
            begin
               for Hop in 1 .. 16 loop
                  exit when not Ctx.Subps.Contains (Cur_K);
                  declare
                     SI : constant Subp_Info := Ctx.Subps.Element (Cur_K);
                     PK : constant Unsigned_32 := SI.Parent_Subp;
                  begin
                     if PK = TI.Parent_Subp then
                        Env_R := SL;
                        exit;
                     elsif PK = 0 or else not Ctx.Subps.Contains (PK) then
                        exit;
                     else
                        declare
                           PI  : constant Subp_Info := Ctx.Subps.Element (PK);
                           FP  : constant String := Next_Reg (Ctx.E, "tslp");
                           NSL : constant String := Next_Reg (Ctx.E, "tsl");
                        begin
                           exit when Length (PI.Frame_Type) = 0;
                           Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (PI.Frame_Type) & ", " & To_String (SL) & ", 0");
                           Emit_Inst (Ctx.E, NSL & " = load.ptr " & FP & ", align 8");
                           SL := To_Unbounded_String (NSL);
                           Cur_K := PK;
                        end;
                     end if;
                  end;
               end loop;
            end;
         end if;
      end if;
      Add_Extern_Func (Ctx.E, To_String (TI.Body_Fn),
        "extern func " & To_String (TI.Body_Fn) & "(%__sl: ptr, %__self: ptr) -> void cc ""c""");
      Emit_Inst (Ctx.E, Fn_Ptr & " = addr " & To_String (TI.Body_Fn));
      declare
         NE_Arg : Unbounded_String := To_Unbounded_String (Img (TI.Num_Entries));

         function Emit_Elab_Flag_Addr return String is
         begin
            if TI.Elab_Is_Local then
               if TI.Elab_Owner = Ctx.Cur_Subp_Key then
                  return To_String (TI.Elab_Flag);
               elsif TI.Elab_Cap_Slot > 0 then
                  declare
                     SP : constant String := Emit_Captured_Slot_Ptr (Ctx, TI.Elab_Owner, Integer (TI.Elab_Cap_Slot));
                  begin
                     if SP /= "0" then
                        declare
                           LP : constant String := Next_Reg (Ctx.E, "tfl");
                        begin
                           Emit_Inst (Ctx.E, LP & " = load.ptr " & SP & ", align 8");
                           return LP;
                        end;
                     end if;
                  end;
               end if;
               if Length (TI.Elab_Fallback) > 0 then
                  declare
                     FA : constant String := Next_Reg (Ctx.E, "tfa");
                  begin
                     Emit_Inst (Ctx.E, FA & " = addr " & To_String (TI.Elab_Fallback));
                     return FA;
                  end;
               end if;
               return "";
            else
               declare
                  FSym : constant String := To_String (TI.Elab_Flag);
                  FA   : constant String := Next_Reg (Ctx.E, "tfa");
               begin
                  Add_Global_Decl (Ctx.E, FSym, "extern " & FSym & " = global i8, align 1");
                  Emit_Inst (Ctx.E, FA & " = addr " & FSym);
                  return FA;
               end;
            end if;
         end Emit_Elab_Flag_Addr;
      begin
         if TI.Has_Dyn_Entries and then Length (TI.Dyn_Entries_Sym) > 0 then
            declare
               DSym : constant String := To_String (TI.Dyn_Entries_Sym);
               DA   : constant String := Next_Reg (Ctx.E, "tnea");
               DV   : constant String := Next_Reg (Ctx.E, "tnev");
            begin
               if TI.Dyn_Entries_Exp then
                  Add_Global_Decl (Ctx.E, DSym, "extern " & DSym & " = global i32, align 4");
               else
                  Add_Global_Decl (Ctx.E, DSym, DSym & " = global i32 = " & Img (TI.Num_Entries) & ", align 4");
               end if;
               Emit_Inst (Ctx.E, DA & " = addr " & DSym);
               Emit_Inst (Ctx.E, DV & " = load.i32 " & DA & ", align 4");
               NE_Arg := To_Unbounded_String (DV);
            end;
         end if;
      if Activate_Now then
         if Ctx.Enable_Checks and then Length (TI.Elab_Flag) > 0 then
            declare
               FA : constant String := Emit_Elab_Flag_Addr;
            begin
               if FA'Length > 0 then
                  declare
                     FV : constant String := Next_Reg (Ctx.E, "tfv");
                     NE : constant String := Next_Reg (Ctx.E, "tne");
                     L_Bad : constant String := Next_Label (Ctx.E, "tpe_b");
                     L_Ok  : constant String := Next_Label (Ctx.E, "tpe_o");
                  begin
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
         declare
            Has_Dyn_SS : constant Boolean :=
              not Is_Void_Tree (TI.Storage_Size_Exp)
              and then Ctx.Saved_Expr_Vals.Contains (Tree_Key (TI.Storage_Size_Exp))
              and then (Ctx.Saved_Expr_Vals.Element (Tree_Key (TI.Storage_Size_Exp)).Is_Global
                        or else Ctx.Saved_Expr_Vals.Element (Tree_Key (TI.Storage_Size_Exp)).Owner_Subp = Ctx.Cur_Subp_Key);
            procedure Emit_Set_Task_SS (TH : String) is
               Dummy_S : Unbounded_String;
            begin
               if TI.Storage_Size > 0 then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_stack_size",
                    TH & ", " & Img (Long_Long_Integer (TI.Storage_Size)), "", Dummy_S, Nounwind => True);
               elsif Has_Dyn_SS then
                  declare
                     SE   : constant Saved_Expr_Info := Ctx.Saved_Expr_Vals.Element (Tree_Key (TI.Storage_Size_Exp));
                     SP   : Unbounded_String := SE.Ptr_Reg;
                     SV_R : constant String := Next_Reg (Ctx.E, "tss");
                     S64  : Unbounded_String;
                  begin
                     if SE.Is_Global then
                        declare
                           GA : constant String := Next_Reg (Ctx.E, "tssa");
                        begin
                           Emit_Inst (Ctx.E, GA & " = addr " & To_String (SE.Ptr_Reg));
                           SP := To_Unbounded_String (GA);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, SV_R & " = load." & To_String (SE.Pir_Type) & " " & To_String (SP) & ", align " & Img (SE.Align));
                     S64 := To_Unbounded_String (Emit_Int_Convert (Ctx, SV_R, To_String (SE.Pir_Type), "i64"));
                     Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_stack_size",
                       TH & ", " & To_String (S64), "", Dummy_S, Nounwind => True);
                  end;
               end if;
            end Emit_Set_Task_SS;
         begin
            if TI.Has_Priority or else TI.Storage_Size > 0 or else Has_Dyn_SS then
               declare
                  Mk    : Unbounded_String;
                  Dummy : Unbounded_String;
               begin
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_mark", To_String (M_Reg), "ptr", Mk, Nounwind => True);
                  Emit_Call_Or_Invoke (Ctx, "@__ada_task_create",
                    To_String (M_Reg) & ", " & Fn_Ptr & ", " & To_String (Env_R) & ", " & To_String (NE_Arg),
                    "ptr", TH_Reg);
                  if TI.Has_Priority then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_priority",
                       To_String (TH_Reg) & ", " & Img (TI.Priority), "", Dummy, Nounwind => True);
                  end if;
                  Emit_Set_Task_SS (To_String (TH_Reg));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_since",
                    To_String (M_Reg) & ", " & To_String (Mk), "", Dummy);
               end;
            else
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_create_and_activate",
                 To_String (M_Reg) & ", " & Fn_Ptr & ", " & To_String (Env_R) & ", " & To_String (NE_Arg),
                 "ptr", TH_Reg);
            end if;
         end;
      else
         Emit_Call_Or_Invoke (Ctx, "@__ada_task_create",
           To_String (M_Reg) & ", " & Fn_Ptr & ", " & To_String (Env_R) & ", " & To_String (NE_Arg),
           "ptr", TH_Reg);
         if TI.Has_Priority then
            declare
               Dummy_P : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_priority",
                 To_String (TH_Reg) & ", " & Img (TI.Priority), "", Dummy_P, Nounwind => True);
            end;
         end if;
         if TI.Storage_Size > 0 then
            declare
               Dummy_S : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_stack_size",
                 To_String (TH_Reg) & ", " & Img (Long_Long_Integer (TI.Storage_Size)), "", Dummy_S, Nounwind => True);
            end;
         elsif not Is_Void_Tree (TI.Storage_Size_Exp)
           and then Ctx.Saved_Expr_Vals.Contains (Tree_Key (TI.Storage_Size_Exp))
           and then (Ctx.Saved_Expr_Vals.Element (Tree_Key (TI.Storage_Size_Exp)).Is_Global
                     or else Ctx.Saved_Expr_Vals.Element (Tree_Key (TI.Storage_Size_Exp)).Owner_Subp = Ctx.Cur_Subp_Key)
         then
            declare
               SE   : constant Saved_Expr_Info := Ctx.Saved_Expr_Vals.Element (Tree_Key (TI.Storage_Size_Exp));
               SP   : Unbounded_String := SE.Ptr_Reg;
               SV_R : constant String := Next_Reg (Ctx.E, "tss");
               S64  : Unbounded_String;
               Dummy_S : Unbounded_String;
            begin
               if SE.Is_Global then
                  declare
                     GA : constant String := Next_Reg (Ctx.E, "tssa");
                  begin
                     Emit_Inst (Ctx.E, GA & " = addr " & To_String (SE.Ptr_Reg));
                     SP := To_Unbounded_String (GA);
                  end;
               end if;
               Emit_Inst (Ctx.E, SV_R & " = load." & To_String (SE.Pir_Type) & " " & To_String (SP) & ", align " & Img (SE.Align));
               S64 := To_Unbounded_String (Emit_Int_Convert (Ctx, SV_R, To_String (SE.Pir_Type), "i64"));
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_stack_size",
                 To_String (TH_Reg) & ", " & To_String (S64), "", Dummy_S, Nounwind => True);
            end;
         end if;
         if Ctx.Cur_Task_Group_Id /= 0 then
            declare
               Dummy_G : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_group",
                 To_String (TH_Reg) & ", " & Img (Long_Long_Integer (Ctx.Cur_Task_Group_Id)), "", Dummy_G, Nounwind => True);
            end;
         end if;
         if Ctx.Enable_Checks and then Length (TI.Elab_Flag) > 0 then
            declare
               FA    : constant String := Emit_Elab_Flag_Addr;
               Dummy : Unbounded_String;
            begin
               if FA'Length > 0 then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_elab_flag",
                    To_String (TH_Reg) & ", " & FA, "", Dummy, Nounwind => True);
               end if;
            end;
         end if;
      end if;
      end;
      return To_String (TH_Reg);
   end Emit_Create_Task_Handle;

   function Is_Shared_Object (Ctx : Context; Node : TREE) return Boolean is
      Cur : TREE := Node;
   begin
      for Step in 1 .. 16 loop
         exit when Is_Void_Tree (Cur);
         if KIND (Cur) = DN_PARENTHESIZED or else KIND (Cur) = DN_CONVERSION then
            Cur := D (AS_EXP, Cur);
         elsif KIND (Cur) = DN_SELECTED then
            declare
               Des : TREE := D (AS_DESIGNATOR, Cur);
               Def : TREE;
            begin
               while not Is_Void_Tree (Des) and then KIND (Des) = DN_SELECTED loop
                  Des := D (AS_DESIGNATOR, Des);
               end loop;
               Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des)));
               if not Is_Void_Tree (Def) and then KIND (Def) /= DN_COMPONENT_ID and then KIND (Def) /= DN_DISCRIMINANT_ID then
                  Cur := Des;
               else
                  return False;
               end if;
            end;
         elsif KIND (Cur) = DN_USED_OBJECT_ID or else KIND (Cur) = DN_USED_NAME_ID then
            declare
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Cur)));
            begin
               if Is_Void_Tree (Def) then
                  return False;
               end if;
               if Ctx.Vars.Contains (Tree_Key (Def)) and then Ctx.Vars.Element (Tree_Key (Def)).Is_Shared then
                  return True;
               end if;
               if KIND (Def) = DN_VARIABLE_ID and then DB (SM_IS_SHARED, Def) then
                  return True;
               end if;
               if KIND (Def) in DN_VARIABLE_ID | DN_CONSTANT_ID
                 and then DB (SM_RENAMES_OBJ, Def)
                 and then not Is_Void_Tree (D (SM_INIT_EXP, Def))
               then
                  Cur := D (SM_INIT_EXP, Def);
               else
                  return False;
               end if;
            end;
         else
            return False;
         end if;
      end loop;
      return False;
   end Is_Shared_Object;

   function Emit_Entry_Index_Reg
     (Ctx     : in out Context;
      EI      : Entry_Info;
      Fam_Exp : TREE) return String
   is
      Base_R : Unbounded_String;
   begin
      if Length (EI.Fam_Base_Sym) > 0 then
         declare
            BSym : constant String := To_String (EI.Fam_Base_Sym);
            BA   : constant String := Next_Reg (Ctx.E, "efba");
            BV   : constant String := Next_Reg (Ctx.E, "efbv");
         begin
            if EI.Fam_Exported then
               Add_Global_Decl (Ctx.E, BSym, "extern " & BSym & " = global i32, align 4");
            else
               Add_Global_Decl (Ctx.E, BSym, BSym & " = global i32 = " & Img (EI.Base_Idx) & ", align 4");
            end if;
            Emit_Inst (Ctx.E, BA & " = addr " & BSym);
            Emit_Inst (Ctx.E, BV & " = load.i32 " & BA & ", align 4");
            Base_R := To_Unbounded_String (BV);
         end;
      else
         declare
            C : constant String := Next_Reg (Ctx.E, "efi");
         begin
            Emit_Inst (Ctx.E, C & " = const.i32 " & Img (EI.Base_Idx));
            Base_R := To_Unbounded_String (C);
         end;
      end if;

      if EI.Is_Family and then not Is_Void_Tree (Fam_Exp) then
         declare
            IV  : constant Expr_Val := Emit_Expr (Ctx, Fam_Exp);
            I32 : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i32",
                                       Is_Unsigned => Is_Unsigned_Scalar (IV.Ada_Type, To_String (IV.Pir_Type)));
            D1  : constant String := Next_Reg (Ctx.E, "efd");
            A1  : constant String := Next_Reg (Ctx.E, "efa");
         begin
            if EI.Fam_Is_Static then
               if Ctx.Enable_Checks then
                  declare
                     LoC : constant String := Next_Reg (Ctx.E, "eflo");
                     HiC : constant String := Next_Reg (Ctx.E, "efhi");
                     Bad : constant String := Next_Reg (Ctx.E, "efb");
                  begin
                     Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i32 " & I32 & ", " & Img (Long_Long_Integer (EI.Fam_Lo)));
                     Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i32 " & I32 & ", " & Img (Long_Long_Integer (EI.Fam_Hi)));
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;
               Emit_Inst (Ctx.E, D1 & " = sub.i32 " & I32 & ", " & Img (Long_Long_Integer (EI.Fam_Lo)));
            else
               declare
                  Lo32, Hi32 : Unbounded_String;
               begin
                  if Length (EI.Fam_Lo_Sym) > 0 and then Length (EI.Fam_Hi_Sym) > 0 then
                     declare
                        LSym : constant String := To_String (EI.Fam_Lo_Sym);
                        HSym : constant String := To_String (EI.Fam_Hi_Sym);
                        LA   : constant String := Next_Reg (Ctx.E, "efla");
                        LV   : constant String := Next_Reg (Ctx.E, "eflv");
                        HA   : constant String := Next_Reg (Ctx.E, "efha");
                        HV   : constant String := Next_Reg (Ctx.E, "efhv");
                     begin
                        if EI.Fam_Exported then
                           Add_Global_Decl (Ctx.E, LSym, "extern " & LSym & " = global i32, align 4");
                           Add_Global_Decl (Ctx.E, HSym, "extern " & HSym & " = global i32, align 4");
                        else
                           Add_Global_Decl (Ctx.E, LSym, LSym & " = global i32 = 0, align 4");
                           Add_Global_Decl (Ctx.E, HSym, HSym & " = global i32 = -1, align 4");
                        end if;
                        Emit_Inst (Ctx.E, LA & " = addr " & LSym);
                        Emit_Inst (Ctx.E, LV & " = load.i32 " & LA & ", align 4");
                        Emit_Inst (Ctx.E, HA & " = addr " & HSym);
                        Emit_Inst (Ctx.E, HV & " = load.i32 " & HA & ", align 4");
                        Lo32 := To_Unbounded_String (LV);
                        Hi32 := To_Unbounded_String (HV);
                     end;
                  else
                     declare
                        RT2, LoR, HiR : Unbounded_String;
                     begin
                        Emit_Discrete_Range (Ctx, EI.Fam_DR, RT2, LoR, HiR);
                        Lo32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LoR), To_String (RT2), "i32"));
                        Hi32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (HiR), To_String (RT2), "i32"));
                     end;
                  end if;
                  if Ctx.Enable_Checks then
                     declare
                        LoC : constant String := Next_Reg (Ctx.E, "eflo");
                        HiC : constant String := Next_Reg (Ctx.E, "efhi");
                        Bad : constant String := Next_Reg (Ctx.E, "efb");
                     begin
                        Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i32 " & I32 & ", " & To_String (Lo32));
                        Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i32 " & I32 & ", " & To_String (Hi32));
                        Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end if;
                  Emit_Inst (Ctx.E, D1 & " = sub.i32 " & I32 & ", " & To_String (Lo32));
               end;
            end if;
            Emit_Inst (Ctx.E, A1 & " = add.i32 " & D1 & ", " & To_String (Base_R));
            return A1;
         end;
      else
         return To_String (Base_R);
      end if;
   end Emit_Entry_Index_Reg;

   function Emit_Duration_Ns
     (Ctx     : in out Context;
      Dur_Exp : TREE) return String
   is
      V   : constant Expr_Val := Emit_Expr (Ctx, Dur_Exp);
      PT  : constant String := To_String (V.Pir_Type);
      F64 : Unbounded_String;
      Sc  : constant String := Next_Reg (Ctx.E, "dns");
      Ns  : constant String := Next_Reg (Ctx.E, "dni");
   begin
      if PT = "f32" or else PT = "f64" then
         F64 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (V.Reg), PT, "f64"));
         Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & To_String (F64) & ", 1.0E+9");
         Emit_Inst (Ctx.E, Ns & " = fptosi.i64 " & Sc & " [sat]");
         return Ns;
      elsif Is_Fixed_Type (V.Ada_Type) then
         declare
            Fl : constant String := Next_Reg (Ctx.E, "dfl");
            Sm : constant IEEE_Float_64 := Fixed_Small (V.Ada_Type) * 1.0E9;
         begin
            Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (V.Reg));
            Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Sm));
            Emit_Inst (Ctx.E, Ns & " = fptosi.i64 " & Sc & " [sat]");
            return Ns;
         end;
      else
         return Emit_Int_Convert (Ctx, To_String (V.Reg), PT, "i64");
      end if;
   end Emit_Duration_Ns;

   procedure Ensure_Enum_Tables_Pkg
     (Ctx       : in out Context;
      Base_Enum : TREE;
      Ptrs_Sym  : out Unbounded_String;
      Count     : out Natural)
   is
      BE : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, Base_Type_Spec (Subst_Tree (Ctx, Base_Enum))));
   begin
      if Is_Void_Tree (BE) or else KIND (BE) /= DN_ENUMERATION then
         declare
            PS : constant String := "@__ada_enum_p_empty";
         begin
            if not Has_Symbol (Ctx.E, PS) then
               Add_Data_Decl (Ctx.E, PS, PS & " = data align 8 { 0, 0, 0, 0, 0, 0, 0, 0 }");
            end if;
            Ptrs_Sym := To_Unbounded_String (PS);
            Count := 0;
            return;
         end;
      end if;
      declare
         Key  : constant String := Tree_Key_Str (BE);
         PS   : constant String := "@__ada_enum_p_" & Key;
         Lits : SEQ_TYPE := LIST (D (SM_LITERAL_S, BE));
         Lit  : TREE;
         Idx  : Natural := 0;
         PBuf : Unbounded_String := To_Unbounded_String (PS & " = data align 8 { ");
      begin
         Ptrs_Sym := To_Unbounded_String (PS);
         Count := 0;
         while not IS_EMPTY (Lits) loop
            POP (Lits, Lit);
            declare
               Raw_Txt : constant String := Sym_Name (Lit);
               Txt : constant String :=
                 (if Is_Character_Type (BE) and then Raw_Txt'Length >= 2 and then Raw_Txt (Raw_Txt'First) = '_'
                  then Raw_Txt (Raw_Txt'First + 1 .. Raw_Txt'Last)
                  else Raw_Txt);
               SS  : constant String := "@__ada_es_" & Key & "_" & Img (Idx);
            begin
               if not Has_Symbol (Ctx.E, PS) then
                  Add_Data_Decl (Ctx.E, SS,
                    SS & " = data align 1 { """ & Escape_Pir_String (Txt) & "\0"" }");
                  if Idx > 0 then
                     Append (PBuf, ", ");
                  end if;
                  Append (PBuf, "ptr " & SS);
               end if;
               Idx := Idx + 1;
            end;
         end loop;
         Count := Idx;
         if not Has_Symbol (Ctx.E, PS) then
            if Idx = 0 then
               Append (PBuf, "0, 0, 0, 0, 0, 0, 0, 0");
            end if;
            Append (PBuf, " }");
            Add_Data_Decl (Ctx.E, PS, To_String (PBuf));
         end if;
      end;
   end Ensure_Enum_Tables_Pkg;


   -- Forwarding wrappers to domain child packages (Cg_Expr.Composite, Cg_Expr.Call)
   procedure Emit_Copy_Composite
     (Ctx      : in out Context;
      Dst_Ptr  : String;
      Src      : Expr_Val;
      Dst_Type : TREE)
   is
   begin
      Cg_Expr.Composite.Emit_Copy_Composite (Ctx, Dst_Ptr, Src, Dst_Type);
   end Emit_Copy_Composite;

   procedure Emit_Init_Composite
     (Ctx          : in out Context;
      Dst_Ptr      : String;
      Dst_Type     : TREE;
      Init_Node    : TREE;
      Dst_Firsts   : Str_Array_4 := (others => Null_Unbounded_String);
      Dst_Lasts    : Str_Array_4 := (others => Null_Unbounded_String);
      Exact_Bounds : Boolean := False)
   is
   begin
      Cg_Expr.Composite.Emit_Init_Composite (Ctx, Dst_Ptr, Dst_Type, Init_Node, Dst_Firsts, Dst_Lasts, Exact_Bounds);
   end Emit_Init_Composite;

   procedure Emit_Default_Init
     (Ctx            : in out Context;
      Dst_Ptr        : String;
      Dst_Type       : TREE;
      Activate_Tasks : Boolean := False)
   is
   begin
      Cg_Expr.Composite.Emit_Default_Init (Ctx, Dst_Ptr, Dst_Type, Activate_Tasks);
   end Emit_Default_Init;

   function Find_Enum_Rep_Type (Ctx : Context; T : TREE) return TREE is
   begin
      return Cg_Expr.Composite.Find_Enum_Rep_Type (Ctx, T);
   end Find_Enum_Rep_Type;

   function Enum_Rep_Has_Negative (BT : TREE) return Boolean is
   begin
      return Cg_Expr.Composite.Enum_Rep_Has_Negative (BT);
   end Enum_Rep_Has_Negative;

   function Emit_Enum_Rep_Table (Ctx : in out Context; BT : TREE; Count : out Natural) return String is
   begin
      return Cg_Expr.Composite.Emit_Enum_Rep_Table (Ctx, BT, Count);
   end Emit_Enum_Rep_Table;

   function Emit_Enum_Pos_To_Rep_I64
     (Ctx    : in out Context;
      BT     : TREE;
      Val_R  : String;
      Pir_Ty : String) return String
   is
   begin
      return Cg_Expr.Composite.Emit_Enum_Pos_To_Rep_I64 (Ctx, BT, Val_R, Pir_Ty);
   end Emit_Enum_Pos_To_Rep_I64;

   function Emit_Enum_Rep_I64_To_Pos
     (Ctx     : in out Context;
      BT      : TREE;
      Rep_I64 : String) return String
   is
   begin
      return Cg_Expr.Composite.Emit_Enum_Rep_I64_To_Pos (Ctx, BT, Rep_I64);
   end Emit_Enum_Rep_I64_To_Pos;

   procedure Emit_Variant_Check_For_Field
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_T     : TREE;
      Field_Def : TREE;
      Use_CK    : Unsigned_32)
   is
   begin
      Cg_Expr.Composite.Emit_Variant_Check_For_Field (Ctx, Rec_Ptr, Rec_T, Field_Def, Use_CK);
   end Emit_Variant_Check_For_Field;

   function Emit_Bitfield_Load
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      FE        : Field_Entry;
      Comp_Type : TREE) return String
   is
   begin
      return Cg_Expr.Composite.Emit_Bitfield_Load (Ctx, Rec_Ptr, FE, Comp_Type);
   end Emit_Bitfield_Load;

   procedure Emit_Bitfield_Store
     (Ctx          : in out Context;
      Rec_Ptr      : String;
      FE           : Field_Entry;
      Val_Reg      : String;
      Val_Pir_Type : String;
      Comp_Type    : TREE)
   is
   begin
      Cg_Expr.Composite.Emit_Bitfield_Store (Ctx, Rec_Ptr, FE, Val_Reg, Val_Pir_Type, Comp_Type);
   end Emit_Bitfield_Store;

   function Emit_Field_Addr
     (Ctx     : in out Context;
      Rec_Ptr : String;
      Rec_Pir : String;
      FE      : Field_Entry;
      Prefix  : String := "fp") return String
   is
   begin
      return Cg_Expr.Composite.Emit_Field_Addr (Ctx, Rec_Ptr, Rec_Pir, FE, Prefix);
   end Emit_Field_Addr;

   function Emit_Load_Field_Scalar
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Comp_Type : TREE;
      Comp_TI   : Type_Info;
      Prefix    : String := "fv") return String
   is
   begin
      return Cg_Expr.Composite.Emit_Load_Field_Scalar (Ctx, Rec_Ptr, Rec_Pir, FE, Comp_Type, Comp_TI, Prefix);
   end Emit_Load_Field_Scalar;

   procedure Emit_Store_Field_Scalar
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Val_Reg   : String;
      Comp_Type : TREE;
      Comp_TI   : Type_Info)
   is
   begin
      Cg_Expr.Composite.Emit_Store_Field_Scalar (Ctx, Rec_Ptr, Rec_Pir, FE, Val_Reg, Comp_Type, Comp_TI);
   end Emit_Store_Field_Scalar;

   function Emit_Load_Field_I64
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Comp_Type : TREE;
      Comp_TI   : Type_Info) return String
   is
   begin
      return Cg_Expr.Composite.Emit_Load_Field_I64 (Ctx, Rec_Ptr, Rec_Pir, FE, Comp_Type, Comp_TI);
   end Emit_Load_Field_I64;

   function Emit_Variant_Choice_Match
     (Ctx       : in out Context;
      DV64      : String;
      Disc_Type : TREE;
      Choice_S  : TREE;
      Is_Others : out Boolean) return String
   is
   begin
      return Cg_Expr.Composite.Emit_Variant_Choice_Match (Ctx, DV64, Disc_Type, Choice_S, Is_Others);
   end Emit_Variant_Choice_Match;

   procedure Emit_Record_Rep_Copy_Into
     (Ctx        : in out Context;
      Dst_Ptr    : String;
      Dst_T      : TREE;
      Src_Ptr    : String;
      Src_T      : TREE;
      Discs_Only : Boolean := False)
   is
   begin
      Cg_Expr.Composite.Emit_Record_Rep_Copy_Into (Ctx, Dst_Ptr, Dst_T, Src_Ptr, Src_T, Discs_Only);
   end Emit_Record_Rep_Copy_Into;

   function Emit_Subp_Call
     (Ctx       : in out Context;
      Call_Node : TREE;
      Is_Func   : Boolean) return Expr_Val
   is
   begin
      return Cg_Expr.Call.Emit_Subp_Call (Ctx, Call_Node, Is_Func);
   end Emit_Subp_Call;

   function Emit_Expr (Ctx : in out Context; Node : TREE; Target_Type : TREE := (0, 0, 0)) return Expr_Val is
      Res : Expr_Val;
      K   : constant NODE_NAME := KIND (Node);
   begin
      -- Return pre-evaluated expression value (e.g. non-discriminant expr in record comp constraint, or 'STORAGE_SIZE: P3-6.6)
      if Ctx.Saved_Expr_Vals.Contains (Tree_Key (Node)) then
         declare
            SE : constant Saved_Expr_Info := Ctx.Saved_Expr_Vals.Element (Tree_Key (Node));
         begin
            if SE.Is_Global or else SE.Owner_Subp = Ctx.Cur_Subp_Key then
               Res.Kind     := EK_Scalar;
               Res.Reg      := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SE.Ptr_Reg), To_String (SE.Pir_Type), SE.Align, SE.Is_Global));
               Res.Pir_Type := SE.Pir_Type;
               Res.Ada_Type := SE.Ada_Type;
               Res.Align    := SE.Align;
               return Res;
            elsif SE.Cap_Slot > 0 and then SE.Owner_Subp /= 0 then
               declare
                  UP : constant String := Emit_Captured_Slot_Ptr (Ctx, SE.Owner_Subp, SE.Cap_Slot);
                  LP : constant String := Next_Reg (Ctx.E, "ssup");
                  LV : constant String := Next_Reg (Ctx.E, "ssul");
               begin
                  Emit_Inst (Ctx.E, LP & " = load.ptr " & UP & ", align 8");
                  Emit_Inst (Ctx.E, LV & " = load." & To_String (SE.Pir_Type) & " " & LP & ", align " & Img (Natural'Max (1, SE.Align)));
                  Res.Kind     := EK_Scalar;
                  Res.Reg      := To_Unbounded_String (LV);
                  Res.Pir_Type := SE.Pir_Type;
                  Res.Ada_Type := SE.Ada_Type;
                  Res.Align    := SE.Align;
                  return Res;
               end;
            end if;
         end;
      end if;
      case K is
         when DN_NUMERIC_LITERAL =>
            declare
               ET  : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               SV  : constant TREE := D (SM_VALUE, Node);
            begin
               if Is_Void_Tree (ET) or else KIND (ET) = DN_UNIVERSAL_INTEGER or else KIND (ET) = DN_UNIVERSAL_REAL then
                  if not Is_Void_Tree (Target_Type) and then Is_Scalar_Type (Target_Type) then
                     ET := Subst_Tree (Ctx, Target_Type);
                  end if;
               end if;
               declare
                  TI : constant Type_Info := Get_Type_Info (Ctx, ET);
                  R  : constant String := Next_Reg (Ctx.E, "c");
               begin
                  Res.Kind     := EK_Scalar;
                  Res.Ada_Type := ET;
                  Res.Pir_Type := TI.Pir_Type;
                  Res.Align    := TI.Align;
                  if Is_Float_Type (ET) then
                     Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (Eval_Float64 (SV)));
                  elsif Is_Fixed_Type (ET) then
                     declare
                        Sc  : Integer_64;
                        Ovf : Boolean;
                     begin
                        Eval_Fixed_Ticks_Exact_Checked (SV, ET, Sc, Ovf);
                        if Ctx.Enable_Checks and then Ovf then
                           Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                           declare
                              Unr : constant String := Next_Label (Ctx.E, "fxo_u");
                           begin
                              Emit_Label (Ctx.E, Unr);
                           end;
                        end if;
                        Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Sc)));
                     end;
                  else
                     declare
                        Val : Integer_64;
                        Ovf : Boolean;
                        RET : constant TREE := Subst_Resolve_Type (Ctx, ET);
                     begin
                        Eval_Int64_Checked (SV, Val, Ovf);
                        if Ctx.Enable_Checks and then not Is_Void_Tree (RET)
                          and then KIND (RET) /= DN_UNIVERSAL_INTEGER
                        then
                           declare
                              BT : constant TREE := Subst_Base_Type (Ctx, RET);
                              StB : Boolean := False;
                              BLo, BHi : Integer_64 := 0;
                           begin
                              if not Is_Void_Tree (BT) and then Is_Discrete_Type (BT) then
                                 Get_Discrete_Bounds (BT, StB, BLo, BHi);
                              end if;
                              if Ovf or else (StB and then (Val < BLo or else Val > BHi)) then
                                 Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                                 declare
                                    Unr : constant String := Next_Label (Ctx.E, "nlo_u");
                                 begin
                                    Emit_Label (Ctx.E, Unr);
                                 end;
                                 Val := 0;
                              end if;
                           end;
                        end if;
                        declare
                           Lit_PT : Unbounded_String := TI.Pir_Type;
                        begin
                           -- P3-1.3: Never truncate a base-type integer literal into a signed 'Size-packed i8/i16/i32!
                           if To_String (Lit_PT) = "i8" and then not Is_Unsigned_Scalar (ET, "i8")
                             and then (Val < -128 or else Val > 127)
                           then
                              if Val >= -32768 and then Val <= 32767 then
                                 Lit_PT := To_Unbounded_String ("i16");
                              elsif Val >= -2147483648 and then Val <= 2147483647 then
                                 Lit_PT := To_Unbounded_String ("i32");
                              else
                                 Lit_PT := To_Unbounded_String ("i64");
                              end if;
                           elsif To_String (Lit_PT) = "i16" and then not Is_Unsigned_Scalar (ET, "i16")
                             and then (Val < -32768 or else Val > 32767)
                           then
                              if Val >= -2147483648 and then Val <= 2147483647 then
                                 Lit_PT := To_Unbounded_String ("i32");
                              else
                                 Lit_PT := To_Unbounded_String ("i64");
                              end if;
                           elsif To_String (Lit_PT) = "i32" and then not Is_Unsigned_Scalar (ET, "i32")
                             and then (Val < -2147483648 or else Val > 2147483647)
                           then
                              Lit_PT := To_Unbounded_String ("i64");
                           end if;
                           Res.Pir_Type := Lit_PT;
                           Emit_Inst (Ctx.E, R & " = const." & To_String (Lit_PT) & " " & Img (Long_Long_Integer (Val)));
                        end;
                     end;
                  end if;
                  Res.Reg := To_Unbounded_String (R);
                  return Res;
               end;
            end;

         when DN_NULL_ACCESS =>
            declare
               ET : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               R  : constant String := Next_Reg (Ctx.E, "nil");
            begin
               Emit_Inst (Ctx.E, R & " = const.ptr 0");
               Res.Kind     := EK_Scalar;
               Res.Reg      := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String ("ptr");
               Res.Ada_Type := ET;
               Res.Align    := 8;
               return Res;
            end;

         when DN_STRING_LITERAL =>
            declare
               Raw  : constant String := Sym_Name (Node);
               Text : constant String := Decode_Ada_String_Lit (Raw);
               Len  : constant Natural := Text'Length;
               Sym  : constant String := "@__ada_str_" & Img (Next_Sym_Id (Ctx.E));
               AR   : constant String := Next_Reg (Ctx.E, "sp");
               ET   : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
            begin
               if Is_Void_Tree (ET) and then not Is_Void_Tree (Target_Type) then
                  ET := Subst_Tree (Ctx, Target_Type);
               end if;
               declare
                  ATI  : constant Type_Info := (if not Is_Void_Tree (ET) then Get_Type_Info (Ctx, ET) else (others => <>));
                  Comp_T : constant TREE := (if ATI.Kind in TK_Constrained_Array | TK_Unconstrained_Array then ATI.Elem_Type else CONST_VOID);
                  Comp_BT : constant TREE := Resolve_Type_Spec (Base_Type_Spec (Comp_T));
                  Comp_TI : constant Type_Info := (if not Is_Void_Tree (Comp_T) then Get_Type_Info (Ctx, Comp_T) else (others => <>));
                  Is_16Bit : constant Boolean := not Is_Void_Tree (Comp_T) and then Comp_TI.Byte_Size = 2;
                  Mapped  : String (1 .. Len) := Text;
                  Mapped16 : String (1 .. Len * 2) := (others => Character'Val (0));
                  Out_Of_Range : Boolean := False;
               begin
                  -- Check user-defined character enum mapping & component subtype bounds (c42006a, P3-3.17)
                  if not Is_Void_Tree (Comp_BT) and then KIND (Comp_BT) = DN_ENUMERATION then
                     declare
                        Is_Std_Char : constant Boolean := Is_Character_Type (Comp_BT) and then Sym_Name (D (XD_SOURCE_NAME, Comp_BT)) = "CHARACTER";
                        C_St : Boolean := False;
                        C_Lo, C_Hi : Integer_64 := 0;
                     begin
                        Get_Discrete_Bounds (Comp_T, C_St, C_Lo, C_Hi);
                        for I in 1 .. Len loop
                           declare
                              Pos : Integer_64 := Integer_64 (Character'Pos (Text (I)));
                           begin
                              if not Is_Std_Char then
                                 -- Look up character literal in SM_LITERAL_S
                                 declare
                                    Lits : SEQ_TYPE := LIST (D (SM_LITERAL_S, Comp_BT));
                                    Lit  : TREE;
                                    Found : Boolean := False;
                                    Target_Lit : constant String := "'" & Text (I) & "'";
                                 begin
                                    while not IS_EMPTY (Lits) loop
                                       POP (Lits, Lit);
                                       if KIND (Lit) = DN_CHARACTER_ID and then Sym_Name (Lit) = Target_Lit then
                                          Pos := Integer_64 (DI (SM_POS, Lit));
                                          Found := True;
                                          exit;
                                       end if;
                                    end loop;
                                    if Found then
                                       if Is_16Bit then
                                          Mapped16 (2 * I - 1) := Character'Val (Natural (Pos mod 256));
                                          Mapped16 (2 * I)     := Character'Val (Natural ((Pos / 256) mod 256));
                                       elsif Pos >= 0 and then Pos <= 255 then
                                          Mapped (I) := Character'Val (Natural (Pos));
                                       else
                                          Out_Of_Range := True;
                                       end if;
                                    else
                                       Out_Of_Range := True;
                                    end if;
                                 end;
                              elsif Is_16Bit then
                                 Mapped16 (2 * I - 1) := Character'Val (Natural (Pos mod 256));
                                 Mapped16 (2 * I)     := Character'Val (Natural ((Pos / 256) mod 256));
                              end if;
                              if C_St then
                                 if Pos < C_Lo or else Pos > C_Hi then
                                    Out_Of_Range := True;
                                 end if;
                              elsif Ctx.Enable_Checks and then not Out_Of_Range then
                                 -- P3-3.17: Dynamic character component subtype bounds check!
                                 declare
                                    CPT : constant String := To_String (Comp_TI.Pir_Type);
                                    CR  : constant String := Next_Reg (Ctx.E, "slc");
                                 begin
                                    Emit_Inst (Ctx.E, CR & " = const." & CPT & " " & Img (Long_Long_Integer (Pos)));
                                    Emit_Range_Check (Ctx, CR, CPT, Comp_T);
                                 end;
                              end if;
                           end;
                        end loop;
                        if Ctx.Enable_Checks and then Out_Of_Range then
                           Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                           declare
                              Unr : constant String := Next_Label (Ctx.E, "sl_u");
                           begin
                              Emit_Label (Ctx.E, Unr);
                           end;
                        end if;
                     end;
                  end if;

                  if Is_16Bit then
                     Add_Data_Decl (Ctx.E, Sym,
                       Sym & " = data align 2 { """ & Escape_Pir_String (Mapped16) & "\0\0"" }");
                  else
                     Add_Data_Decl (Ctx.E, Sym,
                       Sym & " = data align 1 { """ & Escape_Pir_String (Mapped) & "\0"" }");
                  end if;
                  Emit_Inst (Ctx.E, AR & " = addr " & Sym);

                  declare
                     Raw_Idx_T : constant TREE :=
                       (if ATI.Kind in TK_Constrained_Array | TK_Unconstrained_Array and then ATI.NDims >= 1
                        then ATI.Dims (1).Idx_Type else CONST_VOID);
                     Idx_T : TREE := Raw_Idx_T;
                     Idx_Lo, Idx_Hi : Unbounded_String;
                     F_R : Unbounded_String;
                     L_R : Unbounded_String;
                  begin
                     if not Is_Void_Tree (Idx_T) and then Expr_Has_Unbound_Discr (Ctx, Idx_T) then
                        declare
                           BTI : constant Type_Info := Get_Type_Info (Ctx, Base_Type_Spec (ET));
                        begin
                           if BTI.Kind in TK_Constrained_Array | TK_Unconstrained_Array and then BTI.NDims >= 1 then
                              Idx_T := BTI.Dims (1).Idx_Type;
                           else
                              Idx_T := CONST_VOID;
                           end if;
                        end;
                     end if;
                     if not Is_Void_Tree (Idx_T) then
                        Emit_Index_Bounds (Ctx, Idx_T, Idx_Lo, Idx_Hi);
                        F_R := Idx_Lo;
                        declare
                           En64 : constant String := Next_Reg (Ctx.E, "sle6");
                        begin
                           if Len = 0 then
                              -- LRM 4.2(4): null string literal raises CONSTRAINT_ERROR if Idx'First = Base'First
                              if Ctx.Enable_Checks then
                                 declare
                                    BT_St : Boolean := False;
                                    BT_Lo, BT_Hi : Integer_64 := 0;
                                 begin
                                    Get_Discrete_Bounds (Base_Type_Spec (Idx_T), BT_St, BT_Lo, BT_Hi);
                                    if BT_St then
                                       declare
                                          Und : constant String := Next_Reg (Ctx.E, "slund");
                                       begin
                                          Emit_Inst (Ctx.E, Und & " = cmp_le_s.i64 " & To_String (Idx_Lo) & ", " & Img (Long_Long_Integer (BT_Lo)));
                                          Emit_Check_Fail_If (Ctx, Und);
                                       end;
                                    end if;
                                 end;
                              end if;
                              Emit_Inst (Ctx.E, En64 & " = sub.i64 " & To_String (Idx_Lo) & ", 1");
                           else
                              Emit_Inst (Ctx.E, En64 & " = add.i64 " & To_String (Idx_Lo) & ", " & Img (Long_Long_Integer (Len - 1)));
                              if Ctx.Enable_Checks then
                                 declare
                                    Oob : constant String := Next_Reg (Ctx.E, "sloob");
                                 begin
                                    Emit_Inst (Ctx.E, Oob & " = cmp_gt_s.i64 " & En64 & ", " & To_String (Idx_Hi));
                                    Emit_Check_Fail_If (Ctx, Oob);
                                 end;
                              end if;
                           end if;
                           L_R := To_Unbounded_String (En64);
                        end;
                     else
                        declare
                           FR0 : constant String := Next_Reg (Ctx.E, "sf");
                           LR0 : constant String := Next_Reg (Ctx.E, "sl");
                        begin
                           Emit_Inst (Ctx.E, FR0 & " = const.i64 1");
                           Emit_Inst (Ctx.E, LR0 & " = const.i64 " & Img (Len));
                           F_R := To_Unbounded_String (FR0);
                           L_R := To_Unbounded_String (LR0);
                        end;
                     end if;
                     Res.Kind           := EK_Slice;
                     Res.Reg            := To_Unbounded_String (AR);
                     Res.Pir_Type       := To_Unbounded_String ("ptr");
                     Res.Ada_Type       := ET;
                     Res.Align          := 1;
                     Res.NDims          := 1;
                     Res.First_Regs (1) := F_R;
                     Res.Last_Regs (1)  := L_R;
                     return Res;
                  end;
               end;
            end;

         when DN_USED_CHAR | DN_USED_OBJECT_ID =>
            declare
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Node)));
               ET  : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
            begin
               -- Enumeration / Character literal
               if KIND (Def) = DN_ENUMERATION_ID or else KIND (Def) = DN_CHARACTER_ID then
                  declare
                     TI  : constant Type_Info := Get_Type_Info (Ctx, ET);
                     Pos : constant Integer := DI (SM_POS, Def);
                     R   : constant String := Next_Reg (Ctx.E, "en");
                  begin
                     Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Pos));
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  end;
               end if;

               -- Named number (DN_NUMBER_ID) or static constant (e.g. ASCII.LF)
               if KIND (Def) = DN_NUMBER_ID or else Has_Static_Val (D (SM_VALUE, Node)) then
                  declare
                     SV : constant TREE := D (SM_VALUE, Node);
                  begin
                     if Has_Static_Val (SV) then
                        if Is_Void_Tree (ET) or else KIND (ET) = DN_UNIVERSAL_INTEGER or else KIND (ET) = DN_UNIVERSAL_REAL then
                           if not Is_Void_Tree (Target_Type) and then Is_Scalar_Type (Target_Type) then
                              ET := Subst_Tree (Ctx, Target_Type);
                           end if;
                        end if;
                        declare
                           TI     : constant Type_Info := Get_Type_Info (Ctx, ET);
                           R      : constant String := Next_Reg (Ctx.E, "nc");
                           Out_PT : Unbounded_String := TI.Pir_Type;
                        begin
                           if Is_Float_Type (ET) then
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (Eval_Float64 (SV)));
                           elsif Is_Fixed_Type (ET) then
                              declare
                                 Sc  : Integer_64;
                                 Ovf : Boolean;
                              begin
                                 Eval_Fixed_Ticks_Exact_Checked (SV, ET, Sc, Ovf);
                                 if Ctx.Enable_Checks and then Ovf then
                                    Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                                    declare
                                       Unr : constant String := Next_Label (Ctx.E, "fxu_u");
                                    begin
                                       Emit_Label (Ctx.E, Unr);
                                    end;
                                 end if;
                                 Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Sc)));
                              end;
                           else
                              declare
                                 Val : Integer_64;
                                 Ovf : Boolean;
                                 RET : constant TREE := Subst_Resolve_Type (Ctx, ET);
                              begin
                                 Eval_Int64_Checked (SV, Val, Ovf);
                                 if Ctx.Enable_Checks and then not Is_Void_Tree (RET)
                                   and then KIND (RET) /= DN_UNIVERSAL_INTEGER
                                 then
                                    declare
                                       BT : constant TREE := Subst_Base_Type (Ctx, RET);
                                       StB : Boolean := False;
                                       BLo, BHi : Integer_64 := 0;
                                    begin
                                       if not Is_Void_Tree (BT) and then Is_Discrete_Type (BT) then
                                          Get_Discrete_Bounds (BT, StB, BLo, BHi);
                                       end if;
                                       if Ovf or else (StB and then (Val < BLo or else Val > BHi)) then
                                          Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                                          declare
                                             Unr : constant String := Next_Label (Ctx.E, "nuo_u");
                                          begin
                                             Emit_Label (Ctx.E, Unr);
                                          end;
                                          Val := 0;
                                       end if;
                                    end;
                                 end if;
                                 -- P3-1.3: Never truncate a base-type integer literal into a signed 'Size-packed i8/i16/i32!
                                 if To_String (Out_PT) = "i8" and then not Is_Unsigned_Scalar (ET, "i8")
                                   and then (Val < -128 or else Val > 127)
                                 then
                                    if Val >= -32768 and then Val <= 32767 then
                                       Out_PT := To_Unbounded_String ("i16");
                                    elsif Val >= -2147483648 and then Val <= 2147483647 then
                                       Out_PT := To_Unbounded_String ("i32");
                                    else
                                       Out_PT := To_Unbounded_String ("i64");
                                    end if;
                                 elsif To_String (Out_PT) = "i16" and then not Is_Unsigned_Scalar (ET, "i16")
                                   and then (Val < -32768 or else Val > 32767)
                                 then
                                    if Val >= -2147483648 and then Val <= 2147483647 then
                                       Out_PT := To_Unbounded_String ("i32");
                                    else
                                       Out_PT := To_Unbounded_String ("i64");
                                    end if;
                                 elsif To_String (Out_PT) = "i32" and then not Is_Unsigned_Scalar (ET, "i32")
                                   and then (Val < -2147483648 or else Val > 2147483647)
                                 then
                                    Out_PT := To_Unbounded_String ("i64");
                                 end if;
                                 Emit_Inst (Ctx.E, R & " = const." & To_String (Out_PT) & " " & Format_Int_Imm_For_Pir (Val, To_String (Out_PT)));
                              end;
                           end if;
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := Out_PT; Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     elsif KIND (Def) = DN_NUMBER_ID and then not Is_Void_Tree (D (SM_INIT_EXP, Def)) then
                        return Emit_Expr (Ctx, D (SM_INIT_EXP, Def), Target_Type);
                     end if;
                  end;
               end if;

               -- Variable / Parameter / Constant in Var_Maps
               declare
                  Key : constant Unsigned_32 := Tree_Key (Def);
               begin
                  if Ctx.Vars.Contains (Key) then
                     declare
                        V  : constant Var_Info := Ctx.Vars.Element (Key);
                        TI : constant Type_Info := Get_Type_Info (Ctx, V.Ada_Type);
                     begin
                        if V.Kind = VK_Const_Int then
                           Res.Kind := EK_Scalar;
                           Res.Reg := To_Unbounded_String (Emit_Int_Const (Ctx, V.Const_I64, To_String (TI.Pir_Type), "ci"));
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                           return Res;
                        elsif V.Kind = VK_Const_Float then
                           declare
                              R : constant String := Next_Reg (Ctx.E, "cf");
                           begin
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (V.Const_F64));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                              return Res;
                           end;
                        elsif V.Kind = VK_Param_Val then
                           Res.Kind := EK_Scalar; Res.Reg := V.Pir_Name;
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                           return Res;
                        elsif V.Kind = VK_Uncon_Array_Param or else V.Kind = VK_Dyn_Array then
                           Res.Kind     := EK_Slice;
                           Res.Pir_Type := To_Unbounded_String ("ptr");
                           Res.Ada_Type := V.Ada_Type;
                           Res.Align    := V.Align;
                           Res.NDims    := V.NDims;
                           if V.Has_Desc and then
                             (Length (V.Desc_Global) > 0
                              or else (V.Owner_Subp /= 0 and then V.Owner_Subp /= Ctx.Cur_Subp_Key))
                           then
                              declare
                                 DP : Unbounded_String;
                              begin
                                 if Length (V.Desc_Global) > 0 then
                                    declare
                                       GA : constant String := Next_Reg (Ctx.E, "dg");
                                    begin
                                       Emit_Inst (Ctx.E, GA & " = addr " & To_String (V.Desc_Global));
                                       DP := To_Unbounded_String (GA);
                                    end;
                                 else
                                    DP := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                                 end if;
                                 Emit_Unpack_Arr_Desc (Ctx, To_String (DP), V.Ada_Type, Res);
                                 Res.Align := V.Align;
                                 Res.NDims := V.NDims;
                              end;
                           else
                              Res.Reg        := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                              Res.First_Regs := V.First_Regs;
                              Res.Last_Regs  := V.Last_Regs;
                           end if;
                           return Res;
                        elsif TI.Kind = TK_Scalar then
                           declare
                              Addr      : constant String := Emit_Var_Addr (Ctx, V);
                              R         : constant String := Next_Reg (Ctx.E, "ld");
                              Is_Shared : constant Boolean :=
                                V.Is_Shared or else (KIND (Def) = DN_VARIABLE_ID and then DB (SM_IS_SHARED, Def));
                           begin
                              if V.Is_Bitfield then
                                 declare
                                    BV : constant String := Emit_Bitfield_Load (Ctx, Addr, V.BF_FE, V.Ada_Type);
                                 begin
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (BV);
                                    Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                                    return Res;
                                 end;
                              end if;
                              if Is_Shared then
                                 Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & Addr & ", align " & Img (TI.Align) & " [volatile]");
                                 Emit_Inst (Ctx.E, "fence [seq_cst]");
                              else
                                 Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & Addr & ", align " & Img (TI.Align));
                              end if;
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                              return Res;
                           end;
                        else
                           Res.Kind := EK_Addr;
                           Res.Reg  := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                           return Res;
                        end if;
                     end;
                  end if;
               end;

               -- Fallback: external package global via Emit_LValue
               declare
                  LV : constant Expr_Val := Emit_LValue (Ctx, Node);
                  TI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
               begin
                  if TI.Kind = TK_Scalar and then LV.Kind = EK_Addr then
                     declare
                        R         : constant String := Next_Reg (Ctx.E, "gld");
                        Is_Shared : constant Boolean := Is_Shared_Object (Ctx, Node);
                     begin
                        if Is_Shared then
                           Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (TI.Align) & " [volatile]");
                           Emit_Inst (Ctx.E, "fence [seq_cst]");
                        else
                           Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (TI.Align));
                        end if;
                        Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                        Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := LV.Ada_Type; Res.Align := TI.Align;
                        return Res;
                     end;
                  end if;
                  return LV;
               end;
            end;

         when DN_SELECTED =>
            declare
               Des : constant TREE := D (AS_DESIGNATOR, Node);
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des)));
            begin
               -- Package.Constant / Package.Number / Package.Enum_Lit / Package.Object:
               -- Delegate to Emit_Expr so static constants, named numbers, and enum literals work!
               if KIND (Def) /= DN_COMPONENT_ID and then KIND (Def) /= DN_DISCRIMINANT_ID then
                  return Emit_Expr (Ctx, Des, Target_Type);
               end if;
               -- Check for packed bitfield component from DN_RECORD_REP (LRM 13.4)
               declare
                  Pfx_E : constant TREE := D (AS_NAME, Node);
                  Pfx_T_Static : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                  Rec_T_Static : constant TREE :=
                    (if Is_Access_Type (Pfx_T_Static) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T_Static)))
                     else Pfx_T_Static);
                  Dummy_RT : constant Type_Info := Get_Type_Info (Ctx, Rec_T_Static);
                  pragma Unreferenced (Dummy_RT);
                  CK_BF : constant Unsigned_32 := Tree_Key (Def);
               begin
                  if Ctx.Fields.Contains (CK_BF) and then Is_Bitfield (Ctx.Fields.Element (CK_BF)) then
                     declare
                        FE      : constant Field_Entry := Ctx.Fields.Element (CK_BF);
                        Base_LV : constant Expr_Val := Emit_LValue (Ctx, Pfx_E);
                        Rec_Ptr : Unbounded_String := Base_LV.Reg;
                        CT      : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def));
                        CTI     : constant Type_Info := Get_Type_Info (Ctx, CT);
                     begin
                        if Is_Access_Type (Resolve_Type_Spec (Subst_Tree (Ctx, Base_LV.Ada_Type))) then
                           if Base_LV.Kind = EK_Addr then
                              declare
                                 LP : constant String := Next_Reg (Ctx.E, "bfap");
                              begin
                                 if Is_Shared_Object (Ctx, Pfx_E) then
                                    Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8 [volatile]");
                                    Emit_Inst (Ctx.E, "fence [seq_cst]");
                                 else
                                    Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8");
                                 end if;
                                 Rec_Ptr := To_Unbounded_String (LP);
                              end;
                           end if;
                           Emit_Null_Check (Ctx, To_String (Rec_Ptr));
                        end if;
                        if CTI.Kind = TK_Scalar then
                           -- P3-6.3: Run variant discriminant check before bitfield load (LRM 4.1.3(16))
                           if FE.In_Union then
                              Emit_Variant_Check_For_Field (Ctx, To_String (Rec_Ptr), Rec_T_Static, Def, CK_BF);
                           end if;
                           declare
                              VR : constant String := Emit_Bitfield_Load (Ctx, To_String (Rec_Ptr), FE, CT);
                           begin
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (VR);
                              Res.Pir_Type := CTI.Pir_Type; Res.Ada_Type := CT; Res.Align := CTI.Align;
                              return Res;
                           end;
                        end if;
                     end;
                  end if;
               end;
               declare
                  LV : constant Expr_Val := Emit_LValue (Ctx, Node);
                  TI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
               begin
                  if LV.Kind = EK_Addr and then TI.Kind = TK_Scalar then
                     declare
                        R : constant String := Next_Reg (Ctx.E, "ld");
                     begin
                        Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (Natural'Max (1, LV.Align)));
                        Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                        Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := LV.Ada_Type; Res.Align := TI.Align;
                        return Res;
                     end;
                  end if;
                  return LV;
               end;
            end;

         when DN_INDEXED | DN_ALL | DN_SLICE =>
            declare
               LV : constant Expr_Val := Emit_LValue (Ctx, Node);
               TI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
            begin
               if LV.Kind = EK_Addr and then TI.Kind = TK_Scalar then
                  declare
                     R : constant String := Next_Reg (Ctx.E, "ld");
                  begin
                     Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (TI.Align));
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := LV.Ada_Type; Res.Align := TI.Align;
                     return Res;
                  end;
               end if;
               return LV;
            end;

         when DN_FUNCTION_CALL | DN_NULLARY_CALL =>
            return Emit_Subp_Call (Ctx, Node, Is_Func => True);

         when DN_PARENTHESIZED =>
            declare
               Inner_P : constant TREE := D (AS_EXP, Node);
            begin
               -- P3-3.8: Parenthesized array aggregate loses applicable index constraint (LRM 4.3.2(5))
               if not Is_Void_Tree (Inner_P) and then KIND (Inner_P) = DN_AGGREGATE
                 and then not Is_Void_Tree (Target_Type)
                 and then Get_Type_Info (Ctx, Target_Type).Kind in TK_Constrained_Array | TK_Unconstrained_Array
               then
                  return Emit_Expr (Ctx, Inner_P, CONST_VOID);
               end if;
               return Emit_Expr (Ctx, Inner_P, Target_Type);
            end;

         when DN_QUALIFIED =>
            declare
               ET    : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Node)));
               Exp_N : constant TREE := D (AS_EXP, Node);
               E     : Expr_Val := Emit_Expr (Ctx, Exp_N, ET);
               ETI   : constant Type_Info := Get_Type_Info (Ctx, ET);
            begin
               if E.Kind = EK_Scalar then
                  -- P3-2.2: universal_real compound expressions (e.g. Duration'(1.0 / 2.0)) emit f64;
                  -- convert to fixed ticks before Emit_Range_Check!
                  if Is_Fixed_Type (ET) and then (To_String (E.Pir_Type) = "f32" or else To_String (E.Pir_Type) = "f64") then
                     E.Reg      := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (E.Reg), To_String (E.Pir_Type), ET, To_String (ETI.Pir_Type)));
                     E.Pir_Type := ETI.Pir_Type;
                     E.Ada_Type := ET;
                     E.Align    := ETI.Align;
                  end if;
                  Emit_Range_Check (Ctx, To_String (E.Reg), To_String (E.Pir_Type), ET);
                  Emit_Discriminant_Check (Ctx, To_String (E.Reg), ET, Is_Access => True);
               elsif ETI.Kind = TK_Record then
                  Emit_Discriminant_Check (Ctx, To_String (E.Reg), ET, Is_Access => False);
               elsif ETI.Kind = TK_Constrained_Array and then Ctx.Enable_Checks then
                  -- LRM 4.7(3) (c47007a, c48009d/e/f): qualified array checks EXACT bounds (no sliding!),
                  -- except string literals take their lower bound from ET'FIRST (so check length).
                  declare
                     Tgt : Expr_Val;
                     ND_A : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                  begin
                     Tgt.Kind := EK_Slice; Tgt.Ada_Type := ET; Tgt.NDims := ND_A;
                     for D_I in 1 .. ND_A loop
                        Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                     end loop;
                     if not Is_Void_Tree (Exp_N) and then KIND (Exp_N) = DN_STRING_LITERAL then
                        Emit_Array_Length_Check (Ctx, Tgt, E, ND_A);
                     else
                        declare
                           EV_W : constant Array_View := Resolve_Array_View (Ctx, E);
                        begin
                           Emit_Exact_Bounds_Check (Ctx, EV_W.Firsts, EV_W.Lasts, Tgt.First_Regs, Tgt.Last_Regs, ND_A);
                        end;
                     end if;
                  end;
               end if;
               return E;
            end;

         when DN_CONVERSION =>
            declare
               ET  : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               ETI : constant Type_Info := Get_Type_Info (Ctx, ET);
               Inner_E : constant TREE := D (AS_EXP, Node);
            begin
               -- P3-2.4: Exact __int128 fixed-point multiplication/division for Fixed_Target(F1 * F2) / Fixed_Target(F1 / F2)
               -- avoids 53-bit f64 precision loss on 64-bit fixed-point types (e.g. Duration * Duration)!
               if ETI.Kind = TK_Scalar and then Is_Fixed_Type (ET) and then not Is_Void_Tree (Inner_E) then
                  declare
                     U_E : TREE := Inner_E;
                  begin
                     while not Is_Void_Tree (U_E) and then KIND (U_E) = DN_PARENTHESIZED loop
                        U_E := D (AS_EXP, U_E);
                     end loop;
                     if not Is_Void_Tree (U_E) and then KIND (U_E) = DN_FUNCTION_CALL then
                        declare
                           FN_Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, U_E))));
                           PL     : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, U_E));
                           A1, A2 : TREE := CONST_VOID;
                        begin
                           if not Is_Void_Tree (FN_Def) and then KIND (FN_Def) = DN_BLTN_OPERATOR_ID
                             and then (DI (SM_OPERATOR, FN_Def) = 15 or else DI (SM_OPERATOR, FN_Def) = 16)
                           then
                              if not IS_EMPTY (PL) then POP (PL, A1); end if;
                              if not IS_EMPTY (PL) then POP (PL, A2); end if;
                              if not Is_Void_Tree (A1) and then KIND (A1) in DN_ASSOC | DN_NAMED then A1 := D (AS_EXP, A1); end if;
                              if not Is_Void_Tree (A2) and then KIND (A2) in DN_ASSOC | DN_NAMED then A2 := D (AS_EXP, A2); end if;
                              if not Is_Void_Tree (A1) and then not Is_Void_Tree (A2)
                                and then KIND (A1) in CLASS_EXP and then KIND (A2) in CLASS_EXP
                              then
                                 declare
                                    T1 : constant TREE := Subst_Resolve_Type (Ctx, D (SM_EXP_TYPE, A1));
                                    T2 : constant TREE := Subst_Resolve_Type (Ctx, D (SM_EXP_TYPE, A2));
                                    Ok1, Ok2, OkT : Boolean;
                                    N1, D1, N2, D2, NT, DT : Integer_64;
                                 begin
                                    if Is_Fixed_Type (T1) and then Is_Fixed_Type (T2) then
                                       Fixed_Small_Rational (T1, Ok1, N1, D1);
                                       Fixed_Small_Rational (T2, Ok2, N2, D2);
                                       Fixed_Small_Rational (ET, OkT, NT, DT);
                                       if Ok1 and then Ok2 and then OkT then
                                          declare
                                             Is_Mul : constant Boolean := DI (SM_OPERATOR, FN_Def) = 15;
                                             -- Mul: (a*b) * (N1/D1)*(N2/D2)/(NT/DT) = (a*b) * (N1*N2*DT)/(D1*D2*NT)
                                             -- Div: (a/b) * (N1/D1)/(N2/D2)/(NT/DT) = (a/b) * (N1*D2*DT)/(D1*N2*NT)
                                             F1 : Unsigned_64 := Unsigned_64 (N1);
                                             F2 : Unsigned_64 := (if Is_Mul then Unsigned_64 (N2) else Unsigned_64 (D2));
                                             F3 : Unsigned_64 := Unsigned_64 (DT);
                                             G1 : Unsigned_64 := Unsigned_64 (D1);
                                             G2 : Unsigned_64 := (if Is_Mul then Unsigned_64 (D2) else Unsigned_64 (N2));
                                             G3 : Unsigned_64 := Unsigned_64 (NT);
                                             procedure Cancel (X, Y : in out Unsigned_64) is
                                                A : Unsigned_64 := X;
                                                B : Unsigned_64 := Y;
                                                T : Unsigned_64;
                                             begin
                                                while B /= 0 loop T := A mod B; A := B; B := T; end loop;
                                                if A > 1 then X := X / A; Y := Y / A; end if;
                                             end Cancel;
                                          begin
                                             Cancel (F1, G1); Cancel (F1, G2); Cancel (F1, G3);
                                             Cancel (F2, G1); Cancel (F2, G2); Cancel (F2, G3);
                                             Cancel (F3, G1); Cancel (F3, G2); Cancel (F3, G3);
                                             if F1 <= Unsigned_64 (Integer_64'Last) / F2
                                               and then F1 * F2 <= Unsigned_64 (Integer_64'Last) / F3
                                               and then G1 <= Unsigned_64 (Integer_64'Last) / G2
                                               and then G1 * G2 <= Unsigned_64 (Integer_64'Last) / G3
                                             then
                                                declare
                                                   SNum : constant Integer_64 := Integer_64 (F1 * F2 * F3);
                                                   SDen : constant Integer_64 := Integer_64 (G1 * G2 * G3);
                                                   V1   : constant Expr_Val := Emit_Expr (Ctx, A1);
                                                   V2   : constant Expr_Val := Emit_Expr (Ctx, A2);
                                                   A64  : constant String := Emit_Int_Convert (Ctx, To_String (V1.Reg), To_String (V1.Pir_Type), "i64");
                                                   B64  : constant String := Emit_Int_Convert (Ctx, To_String (V2.Reg), To_String (V2.Pir_Type), "i64");
                                                   DPT  : constant String := To_String (ETI.Pir_Type);
                                                   Chk  : constant String := (if Ctx.Enable_Checks then "1" else "0");
                                                   MinB : constant Long_Long_Integer := Min_Signed_For_Pir (DPT);
                                                   MaxB : constant Long_Long_Integer :=
                                                     (if DPT = "i32" then 2147483647 else Long_Long_Integer'Last);
                                                   Fn_S : constant String := (if Is_Mul then "@__ada_fixed_mul_i64" else "@__ada_fixed_div_i64");
                                                   V64  : Unbounded_String;
                                                   R_F  : Unbounded_String;
                                                begin
                                                   Emit_Call_Or_Invoke (Ctx, Fn_S,
                                                     A64 & ", " & B64 & ", " &
                                                     Img (Long_Long_Integer (SNum)) & ", " & Img (Long_Long_Integer (SDen)) & ", " &
                                                     Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                                                   R_F := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", DPT));
                                                   Emit_Range_Check (Ctx, To_String (R_F), DPT, ET);
                                                   Res.Kind     := EK_Scalar;
                                                   Res.Reg      := R_F;
                                                   Res.Pir_Type := ETI.Pir_Type;
                                                   Res.Ada_Type := ET;
                                                   Res.Align    := ETI.Align;
                                                   return Res;
                                                end;
                                             end if;
                                          end;
                                       end if;
                                    end if;
                                 end;
                              end if;
                           end if;
                        end;
                     end if;
                  end;
               end if;
               declare
                  Src : constant Expr_Val := Emit_Expr (Ctx, Inner_E);
                  ST  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Src.Ada_Type));
               begin
               if ETI.Kind /= TK_Scalar then
                  if ETI.Kind = TK_Constrained_Array or else ETI.Kind = TK_Unconstrained_Array then
                     declare
                        STI_C : constant Type_Info := Get_Type_Info (Ctx, Src.Ada_Type);
                        ND : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, Max_Array_Dims));
                        SF, SL : Str_Array_4;
                     begin
                        Emit_Subtypes_Match_Check (Ctx, STI_C.Elem_Type, ETI.Elem_Type);
                        for D_I in 1 .. ND loop
                           Get_Array_Dim_Bounds (Ctx, Src, D_I, SF (D_I), SL (D_I));
                        end loop;
                        Res := Src;
                        Res.Kind     := EK_Slice;
                        Res.Ada_Type := ET;
                        Res.NDims    := ND;
                        if ETI.Kind = TK_Constrained_Array then
                           declare
                              Tgt : Expr_Val;
                           begin
                              Tgt.Kind := EK_Slice; Tgt.Ada_Type := ET; Tgt.NDims := ND;
                              for D_I in 1 .. ND loop
                                 Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                              end loop;
                              Emit_Array_Length_Check (Ctx, Tgt, Src, ND);
                              Res.First_Regs := Tgt.First_Regs;
                              Res.Last_Regs  := Tgt.Last_Regs;
                           end;
                        else
                           if Ctx.Enable_Checks then
                              -- LRM 4.6(10) (c46043b): for each dimension independently, bounds ALWAYS belong to
                              -- target index base type, and for every NON-NULL dimension (SF <= SL) bounds must
                              -- belong to the target index subtype!
                              for D_I in 1 .. ND loop
                                 declare
                                    IT  : constant TREE := ETI.Dims (D_I).Idx_Type;
                                    BIT : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, IT));
                                 begin
                                    if not Is_Void_Tree (BIT) then
                                       Emit_Range_Check (Ctx, To_String (SF (D_I)), "i64", BIT);
                                       Emit_Range_Check (Ctx, To_String (SL (D_I)), "i64", BIT);
                                    end if;
                                    Emit_Non_Null_Index_Check (Ctx, To_String (SF (D_I)), To_String (SL (D_I)), IT);
                                 end;
                              end loop;
                           end if;
                           Res.First_Regs := SF;
                           Res.Last_Regs  := SL;
                        end if;
                        return Res;
                     end;
                  elsif ETI.Kind = TK_Record then
                     -- LRM 13.6: derived record change of representation via canonical Emit_Record_Rep_Copy_Into
                     declare
                        Src_BR : constant TREE := Subst_Base_Type (Ctx, Src.Ada_Type);
                        Dst_BR : constant TREE := Subst_Base_Type (Ctx, ET);
                        Src_RR : constant TREE := (if not Is_Void_Tree (Src_BR) and then KIND (Src_BR) = DN_RECORD then D (SM_REPRESENTATION, Src_BR) else CONST_VOID);
                        Dst_RR : constant TREE := (if not Is_Void_Tree (Dst_BR) and then KIND (Dst_BR) = DN_RECORD then D (SM_REPRESENTATION, Dst_BR) else CONST_VOID);
                     begin
                        if Src_BR /= Dst_BR and then not Is_Void_Tree (Src_BR) and then not Is_Void_Tree (Dst_BR)
                          and then KIND (Src_BR) = DN_RECORD and then KIND (Dst_BR) = DN_RECORD
                          and then (not Is_Void_Tree (Src_RR) or else not Is_Void_Tree (Dst_RR))
                        then
                           declare
                              DTI_R : constant Type_Info := Get_Type_Info (Ctx, Dst_BR);
                              Buf   : constant String := Next_Reg (Ctx.E, "crb");
                              Sz    : constant Natural := Natural'Max (1, DTI_R.Byte_Size);
                              Al    : constant Natural := Natural'Max (1, DTI_R.Align);
                           begin
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Sz) & " x i8], align " & Img (Al));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (Sz) & ", align 1");
                              Emit_Record_Rep_Copy_Into (Ctx, Buf, ET, To_String (Src.Reg), Src.Ada_Type);
                              Emit_Discriminant_Check (Ctx, Buf, ET, Is_Access => False);
                              Res.Kind     := EK_Addr;
                              Res.Reg      := To_Unbounded_String (Buf);
                              Res.Pir_Type := DTI_R.Pir_Type;
                              Res.Ada_Type := ET;
                              Res.Align    := Al;
                              return Res;
                           end;
                        end if;
                     end;
                     Emit_Discriminant_Check (Ctx, To_String (Src.Reg), ET, Is_Access => False);
                     Res := Src;
                     Res.Ada_Type := ET;
                     return Res;
                  else
                     Res := Src;
                     Res.Ada_Type := ET;
                     return Res;
                  end if;
               end if;
               if Is_Access_Type (ET) then
                  Emit_Discriminant_Check (Ctx, To_String (Src.Reg), ET, Is_Access => True);
                  Res.Kind     := EK_Scalar;
                  Res.Reg      := Src.Reg;
                  Res.Ada_Type := ET;
                  Res.Pir_Type := ETI.Pir_Type;
                  Res.Align    := ETI.Align;
                  return Res;
               end if;
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := ETI.Pir_Type;
               Res.Align    := ETI.Align;

               Res.Reg := To_Unbounded_String
                 (Emit_Typed_Scalar_Convert
                    (Ctx, To_String (Src.Reg), ST, To_String (Src.Pir_Type),
                     ET, To_String (ETI.Pir_Type), Check_Range => True));
               return Res;
               end;
            end;

         when DN_SHORT_CIRCUIT =>
            declare
               Is_And : constant Boolean := KIND (D (AS_SHORT_CIRCUIT_OP, Node)) = DN_AND_THEN;
               Res_T  : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               Res_TI : constant Type_Info := Get_Type_Info (Ctx, Res_T);
               L      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Node));
               L_I8   : constant String :=
                 (if To_String (L.Pir_Type) = "i8" then To_String (L.Reg)
                  else Emit_Int_Convert (Ctx, To_String (L.Reg), To_String (L.Pir_Type), "i8", Is_Unsigned => True));
               L_Rhs  : constant String := Next_Label (Ctx.E, "sc_rhs");
               L_End  : constant String := Next_Label (Ctx.E, "sc_end");
               OutR   : constant String := Next_Reg (Ctx.E, "sc");
            begin
               if Is_And then
                  Emit_Term (Ctx.E, "br " & L_I8 & ", " & L_Rhs & ", " & L_End & "(0)");
               else
                  Emit_Term (Ctx.E, "br " & L_I8 & ", " & L_End & "(1), " & L_Rhs);
               end if;
               Emit_Label (Ctx.E, L_Rhs);
               declare
                  R_V  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Node));
                  R_I8 : constant String :=
                    (if To_String (R_V.Pir_Type) = "i8" then To_String (R_V.Reg)
                     else Emit_Int_Convert (Ctx, To_String (R_V.Reg), To_String (R_V.Pir_Type), "i8", Is_Unsigned => True));
               begin
                  Emit_Term (Ctx.E, "jmp " & L_End & "(" & R_I8 & ")");
               end;
               Emit_Label (Ctx.E, L_End, OutR & ": i8");
               Res.Kind     := EK_Scalar;
               if To_String (Res_TI.Pir_Type) /= "i8" and then Res_TI.Kind = TK_Scalar then
                  Res.Reg      := To_Unbounded_String (Emit_Int_Convert (Ctx, OutR, "i8", To_String (Res_TI.Pir_Type), Is_Unsigned => True));
                  Res.Pir_Type := Res_TI.Pir_Type;
                  Res.Align    := Res_TI.Align;
               else
                  Res.Reg      := To_Unbounded_String (OutR);
                  Res.Pir_Type := To_Unbounded_String ("i8");
                  Res.Align    := 1;
               end if;
               Res.Ada_Type := Res_T;
               return Res;
            end;

         when DN_RANGE_MEMBERSHIP =>
            declare
               Is_Not : constant Boolean := KIND (D (AS_MEMBERSHIP_OP, Node)) = DN_NOT_IN;
               V      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Node));
               RT, RL, RH : Unbounded_String;
               VC     : Unbounded_String;
               C1     : constant String := Next_Reg (Ctx.E, "m1");
               C2     : constant String := Next_Reg (Ctx.E, "m2");
               InR    : constant String := Next_Reg (Ctx.E, "min");
            begin
               Emit_Discrete_Range (Ctx, D (AS_RANGE, Node), RT, RL, RH);
               declare
                  PTS  : constant String := To_String (RT);
                  Is_U : constant Boolean := Is_Unsigned_Scalar (V.Ada_Type, PTS);
               begin
                  VC := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), PTS,
                    Is_Unsigned => Is_Unsigned_Scalar (V.Ada_Type, To_String (V.Pir_Type))));
                  if PTS = "f32" or else PTS = "f64" then
                     Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & PTS & " " & To_String (VC) & ", " & To_String (RL));
                     Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & PTS & " " & To_String (VC) & ", " & To_String (RH));
                  elsif Is_U then
                     Emit_Inst (Ctx.E, C1 & " = cmp_ge_u." & PTS & " " & To_String (VC) & ", " & To_String (RL));
                     Emit_Inst (Ctx.E, C2 & " = cmp_le_u." & PTS & " " & To_String (VC) & ", " & To_String (RH));
                  else
                     Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & PTS & " " & To_String (VC) & ", " & To_String (RL));
                     Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & PTS & " " & To_String (VC) & ", " & To_String (RH));
                  end if;
               end;
               Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
               if Is_Not then
                  declare
                     N : constant String := Next_Reg (Ctx.E, "mnot");
                  begin
                     Emit_Inst (Ctx.E, N & " = xor.i8 " & InR & ", 1");
                     Res.Reg := To_Unbounded_String (N);
                  end;
               else
                  Res.Reg := To_Unbounded_String (InR);
               end if;
               Res.Kind     := EK_Scalar;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               Res.Align    := 1;
               return Res;
            end;

         when DN_TYPE_MEMBERSHIP =>
            declare
               Is_Not : constant Boolean := KIND (D (AS_MEMBERSHIP_OP, Node)) = DN_NOT_IN;
               V      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Node));
               TS     : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, Node)))));
               St     : Boolean;
               Lo, Hi : Integer_64;
               InR    : constant String := Next_Reg (Ctx.E, "tmin");
            begin
               Get_Discrete_Bounds (TS, St, Lo, Hi);
               if not Is_Void_Tree (TS) and then To_String (V.Pir_Type) /= "ptr"
                 and then not Is_Access_Type (TS) and then Ctx.Saved_Type_Ranges.Contains (Tree_Key (TS))
                 and then (Ctx.Saved_Type_Ranges.Element (Tree_Key (TS)).Is_Global
                           or else Ctx.Saved_Type_Ranges.Element (Tree_Key (TS)).Owner_Subp = Ctx.Cur_Subp_Key)
               then
                  declare
                     SR  : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (TS));
                     PT  : constant String := To_String (V.Pir_Type);
                     LR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                     HR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                     SRU : constant Boolean := Is_Unsigned_Scalar (TS, To_String (SR.Pir_Type));
                     L_R : constant String := Emit_Scalar_Coerce (Ctx, LR0, To_String (SR.Pir_Type), PT, Is_Unsigned => SRU);
                     H_R : constant String := Emit_Scalar_Coerce (Ctx, HR0, To_String (SR.Pir_Type), PT, Is_Unsigned => SRU);
                     C1  : constant String := Next_Reg (Ctx.E, "tm1");
                     C2  : constant String := Next_Reg (Ctx.E, "tm2");
                  begin
                     if PT = "f32" or else PT = "f64" then
                        Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & PT & " " & To_String (V.Reg) & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & PT & " " & To_String (V.Reg) & ", " & H_R);
                     elsif Is_Unsigned_Scalar (TS, PT) then
                        Emit_Inst (Ctx.E, C1 & " = cmp_ge_u." & PT & " " & To_String (V.Reg) & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = cmp_le_u." & PT & " " & To_String (V.Reg) & ", " & H_R);
                     else
                        Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & PT & " " & To_String (V.Reg) & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & PT & " " & To_String (V.Reg) & ", " & H_R);
                     end if;
                     Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                  end;
               elsif St then
                  declare
                     PT   : constant String := To_String (V.Pir_Type);
                     Is_U : constant Boolean := Is_Unsigned_Scalar (TS, PT);
                     C1   : constant String := Next_Reg (Ctx.E, "tm1");
                     C2   : constant String := Next_Reg (Ctx.E, "tm2");
                  begin
                     if Is_U and then Hi < 0 then
                        Emit_Inst (Ctx.E, InR & " = const.i8 0");
                     elsif Is_U then
                        Emit_Inst (Ctx.E, C1 & " = cmp_ge_u." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Lo)));
                        Emit_Inst (Ctx.E, C2 & " = cmp_le_u." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Hi)));
                        Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                     else
                        Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Lo)));
                        Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Hi)));
                        Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                     end if;
                  end;
               elsif not Is_Void_Tree (TS) and then KIND (TS) in DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED
                 and then not Is_Void_Tree (D (SM_RANGE, TS))
                 and then KIND (D (SM_RANGE, TS)) in DN_RANGE | DN_RANGE_ATTRIBUTE
               then
                  declare
                     Rng : constant TREE := D (SM_RANGE, TS);
                     PT  : constant String := To_String (V.Pir_Type);
                     L_R, H_R : Unbounded_String;
                     C1  : constant String := Next_Reg (Ctx.E, "tm1");
                     C2  : constant String := Next_Reg (Ctx.E, "tm2");

                     function Fixed_Bound_Reg (EV : Expr_Val) return String is
                        SPT : constant String := To_String (EV.Pir_Type);
                     begin
                        if Is_Fixed_Type (TS) and then (SPT = "f64" or else SPT = "f32") then
                           declare
                              F64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), SPT, "f64");
                              Dv  : constant String := Next_Reg (Ctx.E, "fmbd");
                              V64 : Unbounded_String;
                              MinB : constant Long_Long_Integer := (if PT = "i32" then -2147483648 else Long_Long_Integer'First);
                              MaxB : constant Long_Long_Integer := (if PT = "i32" then 2147483647 else Long_Long_Integer'Last);
                           begin
                              Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & F64 & ", " & Format_Float64 (Fixed_Small (TS)));
                              Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                                Dv & ", " & Img (MinB) & ", " & Img (MaxB) & ", 0", "i64", V64);
                              return Emit_Int_Convert (Ctx, To_String (V64), "i64", PT);
                           end;
                        end if;
                        return Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), SPT, PT, Is_Unsigned => Is_Unsigned_Scalar (EV.Ada_Type, SPT));
                     end Fixed_Bound_Reg;
                  begin
                     if KIND (Rng) = DN_RANGE then
                        declare
                           E1 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng), TS);
                           E2 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng), TS);
                        begin
                           L_R := To_Unbounded_String (Fixed_Bound_Reg (E1));
                           H_R := To_Unbounded_String (Fixed_Bound_Reg (E2));
                        end;
                     else
                        declare
                           RT2, LoR, HiR : Unbounded_String;
                           Is_U2 : Boolean;
                        begin
                           Emit_Discrete_Range (Ctx, Rng, RT2, LoR, HiR);
                           Is_U2 := Is_Unsigned_Scalar (TS, To_String (RT2));
                           L_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (LoR), To_String (RT2), PT, Is_Unsigned => Is_U2));
                           H_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (HiR), To_String (RT2), PT, Is_Unsigned => Is_U2));
                        end;
                     end if;
                     if PT = "f32" or else PT = "f64" then
                        Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & PT & " " & To_String (V.Reg) & ", " & To_String (L_R));
                        Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & PT & " " & To_String (V.Reg) & ", " & To_String (H_R));
                     elsif Is_Unsigned_Scalar (TS, PT) then
                        Emit_Inst (Ctx.E, C1 & " = cmp_ge_u." & PT & " " & To_String (V.Reg) & ", " & To_String (L_R));
                        Emit_Inst (Ctx.E, C2 & " = cmp_le_u." & PT & " " & To_String (V.Reg) & ", " & To_String (H_R));
                     else
                        Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & PT & " " & To_String (V.Reg) & ", " & To_String (L_R));
                        Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & PT & " " & To_String (V.Reg) & ", " & To_String (H_R));
                     end if;
                     Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                  end;
               elsif not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_ARRAY then
                  -- LRM 4.5.2(3): array membership checks exact matching of First and Last on every dimension!
                  declare
                     ATI : constant Type_Info := Get_Type_Info (Ctx, TS);
                     ND  : constant Natural := Natural'Max (1, Natural'Min (ATI.NDims, Max_Array_Dims));
                     Acc : Unbounded_String;
                  begin
                     for D_I in 1 .. ND loop
                        declare
                           VF, VL, TF, TL : Unbounded_String;
                           C1 : constant String := Next_Reg (Ctx.E, "am1");
                           C2 : constant String := Next_Reg (Ctx.E, "am2");
                           Both : constant String := Next_Reg (Ctx.E, "amb");
                        begin
                           Get_Array_Dim_Bounds (Ctx, V, D_I, VF, VL);
                           Emit_Index_Bounds (Ctx, ATI.Dims (D_I).Idx_Type, TF, TL);
                           Emit_Inst (Ctx.E, C1 & " = cmp_eq.i64 " & To_String (VF) & ", " & To_String (TF));
                           Emit_Inst (Ctx.E, C2 & " = cmp_eq.i64 " & To_String (VL) & ", " & To_String (TL));
                           Emit_Inst (Ctx.E, Both & " = and.i8 " & C1 & ", " & C2);
                           if D_I = 1 then
                              Acc := To_Unbounded_String (Both);
                           else
                              declare
                                 A2 : constant String := Next_Reg (Ctx.E, "ama");
                              begin
                                 Emit_Inst (Ctx.E, A2 & " = and.i8 " & To_String (Acc) & ", " & Both);
                                 Acc := To_Unbounded_String (A2);
                              end;
                           end if;
                        end;
                     end loop;
                     Emit_Inst (Ctx.E, InR & " = and.i8 " & To_String (Acc) & ", 1");
                  end;
               elsif not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_RECORD then
                  -- LRM 4.5.2(3): record membership compares each discriminant field against TS's constraints
                  declare
                     BT  : constant TREE := Subst_Base_Type (Ctx, TS);
                     RTI : constant Type_Info := Get_Type_Info (Ctx, BT);
                     DS  : constant TREE := (if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then D (SM_DISCRIMINANT_S, BT) else CONST_VOID);
                     ND  : constant TREE := D (SM_NORMALIZED_DSCRMT_S, TS);
                     Acc : Unbounded_String;
                     FIdx : Natural := 0;
                  begin
                     if not Is_Void_Tree (DS) and then not Is_Void_Tree (ND) then
                        declare
                           DL  : SEQ_TYPE := LIST (DS);
                           NDL : SEQ_TYPE := LIST (ND);
                           DD  : TREE;
                        begin
                           while not IS_EMPTY (DL) and then not IS_EMPTY (NDL) loop
                              POP (DL, DD);
                              if KIND (DD) = DN_DSCRMT_DECL then
                                 declare
                                    NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                    DID : TREE;
                                    CE  : TREE;
                                 begin
                                    while not IS_EMPTY (NS) and then not IS_EMPTY (NDL) loop
                                       POP (NS, DID);
                                       POP (NDL, CE);
                                       declare
                                          DT    : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                          DTI   : constant Type_Info := Get_Type_Info (Ctx, DT);
                                          Can_D : constant TREE := Canonical_Def (DID);
                                          DK    : constant Unsigned_32 := Tree_Key (Can_D);
                                          FP    : constant String := Next_Reg (Ctx.E, "rmp");
                                          AV    : constant String := Next_Reg (Ctx.E, "rmv");
                                          ER    : Unbounded_String;
                                          Eq    : constant String := Next_Reg (Ctx.E, "rmq");
                                       begin
                                          if Ctx.Saved_Type_Discs.Contains (Tree_Key (TS))
                                            and then (Ctx.Saved_Type_Discs.Element (Tree_Key (TS)).Is_Global
                                                      or else Ctx.Saved_Type_Discs.Element (Tree_Key (TS)).Owner_Subp = Ctx.Cur_Subp_Key)
                                            and then FIdx + 1 <= Ctx.Saved_Type_Discs.Element (Tree_Key (TS)).Count
                                          then
                                             declare
                                                SD : constant Saved_Disc_Info := Ctx.Saved_Type_Discs.Element (Tree_Key (TS));
                                                LV : constant String := Load_Saved_Slot (Ctx, To_String (SD.Ptrs (FIdx + 1)), To_String (SD.Pir_Types (FIdx + 1)), SD.Aligns (FIdx + 1), SD.Is_Global);
                                             begin
                                                ER := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, LV, To_String (SD.Pir_Types (FIdx + 1)), To_String (DTI.Pir_Type)));
                                             end;
                                          else
                                             declare
                                                EV : constant Expr_Val := Emit_Expr (Ctx, CE, DT);
                                             begin
                                                ER := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), To_String (DTI.Pir_Type)));
                                             end;
                                          end if;
                                          if Ctx.Fields.Contains (DK) then
                                             declare
                                                FE : constant Field_Entry := Ctx.Fields.Element (DK);
                                             begin
                                                if Is_Bitfield (FE) then
                                                   declare
                                                      BV : constant String := Emit_Bitfield_Load (Ctx, To_String (V.Reg), FE, DT);
                                                   begin
                                                      Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & BV & ", " & To_String (ER));
                                                   end;
                                                elsif FE.In_Union or else FE.Has_Explicit_Offset then
                                                   declare
                                                      Off_R : constant String := Next_Reg (Ctx.E, "rmo");
                                                   begin
                                                      Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                                                      Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & To_String (V.Reg) & ", " & Off_R & " [inbounds]");
                                                      Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (Explicit_Field_Align (FE, DTI.Align)));
                                                      Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & To_String (ER));
                                                   end;
                                                else
                                                   Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (V.Reg) & ", " & Img (FE.Field_Idx));
                                                   Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (Natural'Max (1, DTI.Align)));
                                                   Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & To_String (ER));
                                                end if;
                                             end;
                                          else
                                             Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (V.Reg) & ", " & Img (FIdx));
                                             Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (DTI.Align));
                                             Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & To_String (ER));
                                          end if;
                                          if FIdx = 0 then
                                             Acc := To_Unbounded_String (Eq);
                                          else
                                             declare
                                                A2 : constant String := Next_Reg (Ctx.E, "rma");
                                             begin
                                                Emit_Inst (Ctx.E, A2 & " = and.i8 " & To_String (Acc) & ", " & Eq);
                                                Acc := To_Unbounded_String (A2);
                                             end;
                                          end if;
                                       end;
                                       FIdx := FIdx + 1;
                                    end loop;
                                 end;
                              end if;
                           end loop;
                        end;
                     end if;
                     if Length (Acc) > 0 then
                        Emit_Inst (Ctx.E, InR & " = and.i8 " & To_String (Acc) & ", 1");
                     else
                        Emit_Inst (Ctx.E, InR & " = const.i8 1");
                     end if;
                  end;
               elsif not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_ACCESS then
                  -- LRM 4.5.2(3): null satisfies any access subtype; non-null checks designated bounds/discriminants
                  declare
                     DesT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, TS)));
                     IsN  : constant String := Next_Reg (Ctx.E, "amn");
                     L_Do : constant String := Next_Label (Ctx.E, "amd");
                     L_En : constant String := Next_Label (Ctx.E, "ame");
                     Acc  : Unbounded_String;
                  begin
                     Emit_Inst (Ctx.E, IsN & " = cmp_eq.ptr " & To_String (V.Reg) & ", 0");
                     Emit_Term (Ctx.E, "br " & IsN & ", " & L_En & "(1), " & L_Do);
                     Emit_Label (Ctx.E, L_Do);
                     if not Is_Void_Tree (DesT) and then KIND (DesT) = DN_CONSTRAINED_ARRAY
                       and then Is_Access_To_Uncon_Array (TS)
                     then
                        declare
                           ATI : constant Type_Info := Get_Type_Info (Ctx, DesT);
                           ND  : constant Natural := Natural'Max (1, Natural'Min (ATI.NDims, Max_Array_Dims));
                           Unp : Expr_Val;
                        begin
                           Emit_Unpack_Arr_Desc (Ctx, To_String (V.Reg), DesT, Unp);
                           for D_I in 1 .. ND loop
                              declare
                                 TF, TL : Unbounded_String;
                                 C1 : constant String := Next_Reg (Ctx.E, "aa1");
                                 C2 : constant String := Next_Reg (Ctx.E, "aa2");
                                 Bo : constant String := Next_Reg (Ctx.E, "aab");
                              begin
                                 Emit_Index_Bounds (Ctx, ATI.Dims (D_I).Idx_Type, TF, TL);
                                 Emit_Inst (Ctx.E, C1 & " = cmp_eq.i64 " & To_String (Unp.First_Regs (D_I)) & ", " & To_String (TF));
                                 Emit_Inst (Ctx.E, C2 & " = cmp_eq.i64 " & To_String (Unp.Last_Regs (D_I)) & ", " & To_String (TL));
                                 Emit_Inst (Ctx.E, Bo & " = and.i8 " & C1 & ", " & C2);
                                 if D_I = 1 then Acc := To_Unbounded_String (Bo);
                                 else
                                    declare
                                       A2 : constant String := Next_Reg (Ctx.E, "aaa");
                                    begin
                                       Emit_Inst (Ctx.E, A2 & " = and.i8 " & To_String (Acc) & ", " & Bo);
                                       Acc := To_Unbounded_String (A2);
                                    end;
                                 end if;
                              end;
                           end loop;
                        end;
                     elsif not Is_Void_Tree (DesT) and then KIND (DesT) = DN_CONSTRAINED_RECORD then
                        declare
                           BT  : constant TREE := Subst_Base_Type (Ctx, DesT);
                           RTI : constant Type_Info := Get_Type_Info (Ctx, BT);
                           DS  : constant TREE := (if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then D (SM_DISCRIMINANT_S, BT) else CONST_VOID);
                           ND  : constant TREE := D (SM_NORMALIZED_DSCRMT_S, DesT);
                           FIdx : Natural := 0;
                        begin
                           if not Is_Void_Tree (DS) and then not Is_Void_Tree (ND) then
                              declare
                                 DL  : SEQ_TYPE := LIST (DS);
                                 NDL : SEQ_TYPE := LIST (ND);
                                 DD  : TREE;
                              begin
                                 while not IS_EMPTY (DL) and then not IS_EMPTY (NDL) loop
                                    POP (DL, DD);
                                    if KIND (DD) = DN_DSCRMT_DECL then
                                       declare
                                          NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                          DID : TREE;
                                          CE  : TREE;
                                       begin
                                          while not IS_EMPTY (NS) and then not IS_EMPTY (NDL) loop
                                             POP (NS, DID);
                                             POP (NDL, CE);
                                             declare
                                                DT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                                DTI : constant Type_Info := Get_Type_Info (Ctx, DT);
                                                Can_D : constant TREE := Canonical_Def (DID);
                                                DK  : constant Unsigned_32 := Tree_Key (Can_D);
                                                FP  : constant String := Next_Reg (Ctx.E, "arp");
                                                AV  : constant String := Next_Reg (Ctx.E, "arv");
                                                ER  : Unbounded_String;
                                                Eq  : constant String := Next_Reg (Ctx.E, "arq");
                                             begin
                                                if Ctx.Saved_Type_Discs.Contains (Tree_Key (DesT))
                                                  and then (Ctx.Saved_Type_Discs.Element (Tree_Key (DesT)).Is_Global
                                                            or else Ctx.Saved_Type_Discs.Element (Tree_Key (DesT)).Owner_Subp = Ctx.Cur_Subp_Key)
                                                  and then FIdx + 1 <= Ctx.Saved_Type_Discs.Element (Tree_Key (DesT)).Count
                                                then
                                                   declare
                                                      SD : constant Saved_Disc_Info := Ctx.Saved_Type_Discs.Element (Tree_Key (DesT));
                                                      LV : constant String := Load_Saved_Slot (Ctx, To_String (SD.Ptrs (FIdx + 1)), To_String (SD.Pir_Types (FIdx + 1)), SD.Aligns (FIdx + 1), SD.Is_Global);
                                                   begin
                                                      ER := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, LV, To_String (SD.Pir_Types (FIdx + 1)), To_String (DTI.Pir_Type)));
                                                   end;
                                                else
                                                   declare
                                                      EV : constant Expr_Val := Emit_Expr (Ctx, CE, DT);
                                                   begin
                                                      ER := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), To_String (DTI.Pir_Type)));
                                                   end;
                                                end if;
                                                if Ctx.Fields.Contains (DK) then
                                                   declare
                                                      FE : constant Field_Entry := Ctx.Fields.Element (DK);
                                                   begin
                                                      if Is_Bitfield (FE) then
                                                         declare
                                                            BV : constant String := Emit_Bitfield_Load (Ctx, To_String (V.Reg), FE, DT);
                                                         begin
                                                            Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & BV & ", " & To_String (ER));
                                                         end;
                                                      elsif FE.In_Union or else FE.Has_Explicit_Offset then
                                                         declare
                                                            Off_R : constant String := Next_Reg (Ctx.E, "aro");
                                                         begin
                                                            Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                                                            Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & To_String (V.Reg) & ", " & Off_R & " [inbounds]");
                                                            Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (Explicit_Field_Align (FE, DTI.Align)));
                                                            Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & To_String (ER));
                                                         end;
                                                      else
                                                         Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (V.Reg) & ", " & Img (FE.Field_Idx));
                                                         Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (Natural'Max (1, DTI.Align)));
                                                         Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & To_String (ER));
                                                      end if;
                                                   end;
                                                else
                                                   Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (V.Reg) & ", " & Img (FIdx));
                                                   Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (DTI.Align));
                                                   Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & To_String (ER));
                                                end if;
                                                if FIdx = 0 then Acc := To_Unbounded_String (Eq);
                                                else
                                                   declare
                                                      A2 : constant String := Next_Reg (Ctx.E, "ara");
                                                   begin
                                                      Emit_Inst (Ctx.E, A2 & " = and.i8 " & To_String (Acc) & ", " & Eq);
                                                      Acc := To_Unbounded_String (A2);
                                                   end;
                                                end if;
                                             end;
                                             FIdx := FIdx + 1;
                                          end loop;
                                       end;
                                    end if;
                                 end loop;
                              end;
                           end if;
                        end;
                     end if;
                     if Length (Acc) = 0 then
                        Acc := To_Unbounded_String ("1");
                     end if;
                     Emit_Term (Ctx.E, "jmp " & L_En & "(" & To_String (Acc) & ")");
                     Emit_Label (Ctx.E, L_En, InR & ": i8");
                  end;
               else
                  Emit_Inst (Ctx.E, InR & " = const.i8 1");
               end if;
               if Is_Not then
                  declare
                     N : constant String := Next_Reg (Ctx.E, "tmnot");
                  begin
                     Emit_Inst (Ctx.E, N & " = xor.i8 " & InR & ", 1");
                     Res.Reg := To_Unbounded_String (N);
                  end;
               else
                  Res.Reg := To_Unbounded_String (InR);
               end if;
               Res.Kind     := EK_Scalar;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               Res.Align    := 1;
               return Res;
            end;

         when DN_ATTRIBUTE =>
            return Emit_Attribute (Ctx, Node, Target_Type);

         when DN_AGGREGATE =>
            declare
               ET  : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
            begin
               if Is_Void_Tree (ET) or else Is_Unconstrained_Array (ET) then
                  if not Is_Void_Tree (Target_Type) then
                     ET := Subst_Tree (Ctx, Target_Type);
                  end if;
               end if;
               declare
                  TI  : constant Type_Info := Get_Type_Info (Ctx, ET);
               begin
                  if TI.Kind = TK_Unconstrained_Array
                    or else TI.Kind = TK_Constrained_Array
                  then
                     -- Dynamic / unconstrained array aggregate (1D..4D) in expression position:
                     -- compute bounds across all dims, allocate on Secondary Stack, initialize, return EK_Slice!
                     declare
                        ETI    : constant Type_Info := Get_Type_Info (Ctx, TI.Elem_Type);
                        Is_DE  : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
                        EAl    : constant Natural := (if Is_DE then 8 else Natural'Max (1, ETI.Align));
                        ESz    : constant Natural := Elem_Storage_Size (ETI);
                        ND     : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, Max_Array_Dims));
                        Firsts : Str_Array_4;
                        Lasts  : Str_Array_4;
                        Tot64  : Unbounded_String;
                        Byt    : constant String := Next_Reg (Ctx.E, "agby");
                        Buf_U  : Unbounded_String;
                        Has_Others : Boolean := False;
                        Temp_Rng_Keys : U32_Vecs.Vector;
                        Temp_Exp_Keys : U32_Vecs.Vector;
                        Base_TI       : constant Type_Info := Get_Type_Info (Ctx, Resolve_Type_Spec (Base_Type_Spec (ET)));
                     begin
                        -- Check if aggregate (or any nested array subaggregate up to ND dims) uses OTHERS
                        declare
                           function Rec_Has_Others (AN : TREE; Rem_D : Natural) return Boolean is
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
                                       if Rem_D > 1 and then Rec_Has_Others (D (AS_EXP, IT), Rem_D - 1) then
                                          return True;
                                       end if;
                                    else
                                       if Rem_D > 1 and then Rec_Has_Others (IT, Rem_D - 1) then
                                          return True;
                                       end if;
                                    end if;
                                 end loop;
                              end;
                              return False;
                           end Rec_Has_Others;
                        begin
                           Has_Others := Rec_Has_Others (Node, ND);
                        end;

                        if TI.Kind = TK_Constrained_Array and then Has_Others then
                           for D_I in 1 .. ND loop
                              Emit_Index_Bounds (Ctx, TI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                           end loop;
                        else
                           declare
                              Cur_Agg : TREE := Node;
                           begin
                              for Dim_I in 1 .. ND loop
                                 declare
                                    Assocs : SEQ_TYPE :=
                                      (if not Is_Void_Tree (Cur_Agg) and then KIND (Cur_Agg) = DN_AGGREGATE
                                       then LIST (D (AS_GENERAL_ASSOC_S, Cur_Agg)) else MAKE);
                                    Item   : TREE;
                                    Count  : Integer_64 := 0;
                                    Got_R  : Boolean := False;
                                    Next_A : TREE := CONST_VOID;
                                 begin
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
                                                      L64, H64   : Unbounded_String;
                                                      SR : Saved_Range_Info;
                                                      RU : Boolean;
                                                   begin
                                                      Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                                      RU := Dim_I <= Base_TI.NDims
                                                        and then not Is_Void_Tree (Base_TI.Dims (Dim_I).Idx_Type)
                                                        and then Is_Unsigned_Scalar (Base_TI.Dims (Dim_I).Idx_Type, To_String (RT));
                                                      L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64", Is_Unsigned => RU));
                                                      H64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64", Is_Unsigned => RU));
                                                      -- Cache Pass-1 choice range so Pass 2 does NOT re-evaluate side effects (c43207b/c, c43208b)
                                                      Save_Scalar_Slot (Ctx, To_String (L64), "i64", 8, SR.Lo_Ptr, SR.Is_Global);
                                                      Save_Scalar_Slot (Ctx, To_String (H64), "i64", 8, SR.Hi_Ptr, SR.Is_Global);
                                                      SR.Pir_Type   := To_Unbounded_String ("i64");
                                                      SR.Align      := 8;
                                                      SR.Owner_Subp := Ctx.Cur_Subp_Key;
                                                      Ctx.Saved_Type_Ranges.Include (Tree_Key (Ch), SR);
                                                      Temp_Rng_Keys.Append (Tree_Key (Ch));
                                                      -- Check non-null choice range against BASE index subtype even if outer dim is null (c43206a C1)
                                                      if Ctx.Enable_Checks and then Dim_I <= Base_TI.NDims and then not Is_Void_Tree (Base_TI.Dims (Dim_I).Idx_Type) then
                                                         Emit_Non_Null_Index_Check (Ctx, To_String (L64), To_String (H64), Base_TI.Dims (Dim_I).Idx_Type);
                                                      end if;
                                                      if not Got_R then
                                                         Firsts (Dim_I) := L64;
                                                         Lasts (Dim_I)  := H64;
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "amn");
                                                            ML : constant String := Next_Reg (Ctx.E, "alo");
                                                            CH : constant String := Next_Reg (Ctx.E, "amx");
                                                            MH : constant String := Next_Reg (Ctx.E, "ahi");
                                                         begin
                                                            Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i64 " & To_String (L64) & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, ML & " = select.i64 " & CL & ", " & To_String (L64) & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i64 " & To_String (H64) & ", " & To_String (Lasts (Dim_I)));
                                                            Emit_Inst (Ctx.E, MH & " = select.i64 " & CH & ", " & To_String (H64) & ", " & To_String (Lasts (Dim_I)));
                                                            Firsts (Dim_I) := To_Unbounded_String (ML);
                                                            Lasts (Dim_I)  := To_Unbounded_String (MH);
                                                         end;
                                                      end if;
                                                   end;
                                                elsif KIND (Ch) = DN_CHOICE_EXP then
                                                   declare
                                                      ChE  : constant TREE := D (AS_EXP, Ch);
                                                      CE   : constant Expr_Val := Emit_Expr (Ctx, ChE);
                                                      CE_U : constant Boolean := Is_Unsigned_Scalar (CE.Ada_Type, To_String (CE.Pir_Type));
                                                      C64  : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i64", Is_Unsigned => CE_U);
                                                      SE   : Saved_Expr_Info;
                                                   begin
                                                      if CE.Kind = EK_Scalar then
                                                         Save_Scalar_Slot (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), CE.Align, SE.Ptr_Reg, SE.Is_Global);
                                                         SE.Pir_Type   := CE.Pir_Type;
                                                         SE.Ada_Type   := CE.Ada_Type;
                                                         SE.Align      := CE.Align;
                                                         SE.Owner_Subp := Ctx.Cur_Subp_Key;
                                                         Ctx.Saved_Expr_Vals.Include (Tree_Key (ChE), SE);
                                                         Temp_Exp_Keys.Append (Tree_Key (ChE));
                                                      end if;
                                                      if Ctx.Enable_Checks and then Dim_I <= Base_TI.NDims and then not Is_Void_Tree (Base_TI.Dims (Dim_I).Idx_Type) then
                                                         Emit_Range_Check (Ctx, C64, "i64", Base_TI.Dims (Dim_I).Idx_Type);
                                                      end if;
                                                      if not Got_R then
                                                         Firsts (Dim_I) := To_Unbounded_String (C64);
                                                         Lasts (Dim_I)  := To_Unbounded_String (C64);
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "amn");
                                                            ML : constant String := Next_Reg (Ctx.E, "alo");
                                                            CH : constant String := Next_Reg (Ctx.E, "amx");
                                                            MH : constant String := Next_Reg (Ctx.E, "ahi");
                                                         begin
                                                            Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i64 " & C64 & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, ML & " = select.i64 " & CL & ", " & C64 & ", " & To_String (Firsts (Dim_I)));
                                                            Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i64 " & C64 & ", " & To_String (Lasts (Dim_I)));
                                                            Emit_Inst (Ctx.E, MH & " = select.i64 " & CH & ", " & C64 & ", " & To_String (Lasts (Dim_I)));
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
                                       if not Is_Void_Tree (Cur_Agg) and then KIND (Cur_Agg) = DN_STRING_LITERAL then
                                          declare
                                             STxt : constant String := Decode_Ada_String_Lit (Sym_Name (Cur_Agg));
                                          begin
                                             Count := Integer_64 (STxt'Length);
                                          end;
                                       end if;
                                       declare
                                          Idx_Lo_R, Idx_Hi_R : Unbounded_String;
                                          En64 : constant String := Next_Reg (Ctx.E, "ae64");
                                       begin
                                          Emit_Index_Bounds (Ctx, TI.Dims (Dim_I).Idx_Type, Idx_Lo_R, Idx_Hi_R);
                                          Emit_Inst (Ctx.E, En64 & " = add.i64 " & To_String (Idx_Lo_R) & ", " & Img (Long_Long_Integer (Count - 1)));
                                          if Ctx.Enable_Checks and then Count > 0 then
                                             declare
                                                Oob : constant String := Next_Reg (Ctx.E, "agoob");
                                             begin
                                                Emit_Inst (Ctx.E, Oob & " = cmp_gt_s.i64 " & En64 & ", " & To_String (Idx_Hi_R));
                                                Emit_Check_Fail_If (Ctx, Oob);
                                             end;
                                          end if;
                                          Firsts (Dim_I) := Idx_Lo_R;
                                          Lasts (Dim_I)  := To_Unbounded_String (En64);
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
                           Byt_U : Unbounded_String;
                           EAl   : constant Natural := Elem_Storage_Align (ETI);
                        begin
                           Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND, ESz, Tot64, Byt_U);
                           Emit_Inst (Ctx.E, Byt & " = add.i64 " & To_String (Byt_U) & ", 0");
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Byt & ", " & Img (EAl), "ptr", Buf_U);
                           Emit_Init_Composite (Ctx, To_String (Buf_U), ET, Node, Firsts, Lasts);
                           for K_Tmp of Temp_Rng_Keys loop
                              Ctx.Saved_Type_Ranges.Exclude (K_Tmp);
                           end loop;
                           for K_Tmp of Temp_Exp_Keys loop
                              Ctx.Saved_Expr_Vals.Exclude (K_Tmp);
                           end loop;
                           Res.Kind       := EK_Slice;
                           Res.Reg        := Buf_U;
                           Res.Pir_Type   := To_Unbounded_String ("ptr");
                           Res.Ada_Type   := ET;
                           Res.Align      := EAl;
                           Res.NDims      := ND;
                           Res.First_Regs := Firsts;
                           Res.Last_Regs  := Lasts;
                           return Res;
                        end;
                     end;
                  elsif TI.Kind = TK_Void then
                     declare
                        R : constant String := Next_Reg (Ctx.E, "aggv");
                     begin
                        Emit_Inst (Ctx.E, R & " = const.ptr 0");
                        Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (R);
                        Res.Pir_Type := To_Unbounded_String ("ptr"); Res.Ada_Type := ET; Res.Align := 1;
                        return Res;
                     end;
                  else
                     declare
                        Tmp : constant String := Next_Reg (Ctx.E, "aggtmp");
                     begin
                        Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                        Emit_Init_Composite (Ctx, Tmp, ET, Node);
                        Res.Kind     := EK_Addr;
                        Res.Reg      := To_Unbounded_String (Tmp);
                        Res.Pir_Type := TI.Pir_Type;
                        Res.Ada_Type := ET;
                        Res.Align    := TI.Align;
                        return Res;
                     end;
                  end if;
               end;
            end;

         when DN_QUALIFIED_ALLOCATOR | DN_SUBTYPE_ALLOCATOR =>
            declare
               ET     : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Node)));
               Des_T  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, ET)));
               Alloc_T : TREE := Des_T;
               Ptr_R  : Unbounded_String;

               -- Resolve master for `new` allocator: prefer the access type's declaring master (LRM 9.4)
               function Resolve_Alloc_Master return Unbounded_String is
                  Cur : TREE := Base_Type_Spec (ET);
               begin
                  for Step in 1 .. 16 loop
                     exit when Is_Void_Tree (Cur) or else KIND (Cur) /= DN_ACCESS;
                     if Ctx.Access_Masters.Contains (Tree_Key (Cur)) then
                        declare
                           AMI : constant Access_Master_Info := Ctx.Access_Masters.Element (Tree_Key (Cur));
                           MV  : constant String := Next_Reg (Ctx.E, "amv");
                        begin
                           if Length (AMI.Local_Ptr) > 0 then
                              if AMI.Owner_Subp = Ctx.Cur_Subp_Key then
                                 Emit_Inst (Ctx.E, MV & " = load.ptr " & To_String (AMI.Local_Ptr) & ", align 8");
                                 return To_Unbounded_String (MV);
                              else
                                 -- P3-4.3: Prefer static-link frame capture over global mirror to avoid concurrent/recursive races
                                 if AMI.Cap_Slot > 0 then
                                    declare
                                       SP : constant String :=
                                         Emit_Captured_Slot_Ptr (Ctx, AMI.Owner_Subp, AMI.Cap_Slot);
                                    begin
                                       if SP /= "0" then
                                          declare
                                             MP : constant String := Next_Reg (Ctx.E, "amp");
                                          begin
                                             Emit_Inst (Ctx.E, MP & " = load.ptr " & SP & ", align 8");
                                             Emit_Inst (Ctx.E, MV & " = load.ptr " & MP & ", align 8");
                                             return To_Unbounded_String (MV);
                                          end;
                                       end if;
                                    end;
                                 end if;
                                 if Length (AMI.Global_Sym) > 0 then
                                    declare
                                       MA : constant String := Next_Reg (Ctx.E, "ama");
                                    begin
                                       Emit_Inst (Ctx.E, MA & " = addr " & To_String (AMI.Global_Sym));
                                       Emit_Inst (Ctx.E, MV & " = load.ptr " & MA & ", align 8");
                                       return To_Unbounded_String (MV);
                                    end;
                                 end if;
                              end if;
                           elsif Length (AMI.Global_Sym) > 0 then
                              declare
                                 MA : constant String := Next_Reg (Ctx.E, "ama");
                              begin
                                 Emit_Inst (Ctx.E, MA & " = addr " & To_String (AMI.Global_Sym));
                                 Emit_Inst (Ctx.E, MV & " = load.ptr " & MA & ", align 8");
                                 return To_Unbounded_String (MV);
                              end;
                           end if;
                        end;
                     end if;
                     Cur := Resolve_Type_Spec (D (SM_DERIVED, Cur));
                  end loop;
                  declare
                     Root_A : TREE := Base_Type_Spec (ET);
                     Src_A  : TREE := CONST_VOID;
                  begin
                     for Step in 1 .. 16 loop
                        exit when Is_Void_Tree (Root_A) or else KIND (Root_A) /= DN_ACCESS;
                        exit when Is_Void_Tree (D (SM_DERIVED, Root_A));
                        Root_A := Resolve_Type_Spec (D (SM_DERIVED, Root_A));
                     end loop;
                     if not Is_Void_Tree (Root_A) and then KIND (Root_A) = DN_ACCESS then
                        Src_A := D (XD_SOURCE_NAME, Root_A);
                     end if;
                     if not Is_Void_Tree (Src_A) and then Is_Library_Level (Src_A) then
                        declare
                           GM : Unbounded_String;
                        begin
                           Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM, Nounwind => True);
                           return GM;
                        end;
                     end if;
                  end;
                  if Length (Ctx.Cur_Master_Reg) > 0 then
                     return Ctx.Cur_Master_Reg;
                  end if;
                  declare
                     GM : Unbounded_String;
                  begin
                     Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", GM, Nounwind => True);
                     return GM;
                  end;
               end Resolve_Alloc_Master;

               procedure Emit_Acc_Alloc (Sz_I64 : String; Out_Ptr : out Unbounded_String) is
                  Cur  : TREE := Base_Type_Spec (ET);
                  SS_E : TREE := CONST_VOID;
               begin
                  for Step in 1 .. 16 loop
                     exit when Is_Void_Tree (Cur) or else KIND (Cur) /= DN_ACCESS;
                     SS_E := D (SM_STORAGE_SIZE, Cur);
                     exit when not Is_Void_Tree (SS_E);
                     Cur := Resolve_Type_Spec (D (SM_DERIVED, Cur));
                  end loop;
                  if not Is_Void_Tree (SS_E) then
                     declare
                        Cur_N : constant TREE := D (XD_SOURCE_NAME, Cur);
                        Is_Lib_Pool : constant Boolean := not Is_Void_Tree (Cur_N) and then Is_Library_Level (Cur_N);
                        PSym : constant String :=
                          (if Is_Lib_Pool then "@ada_pool_used__" & Qualified_Ada_Name (Cur_N)
                           else "@ada_pool_used_" & Tree_Key_Str (Cur) & To_String (Ctx.Gen_Inst_Tag));
                        UP   : constant String := Next_Reg (Ctx.E, "pup");
                        L64  : Unbounded_String;
                     begin
                        if Is_Lib_Pool and then not Ctx.Saved_Expr_Vals.Contains (Tree_Key (SS_E))
                          and then not Has_Static_Val (Safe_SM_Value (SS_E))
                        then
                           declare
                              SSym : constant String := "@ada_ssz__" & Qualified_Ada_Name (Cur_N);
                              SGA  : constant String := Next_Reg (Ctx.E, "sspa");
                              SGV  : constant String := Next_Reg (Ctx.E, "sspv");
                           begin
                              Add_Global_Decl (Ctx.E, SSym, "extern " & SSym & " = global i64, align 8");
                              Emit_Inst (Ctx.E, SGA & " = addr " & SSym);
                              Emit_Inst (Ctx.E, SGV & " = load.i64 " & SGA & ", align 8");
                              L64 := To_Unbounded_String (SGV);
                           end;
                        else
                           declare
                              LV : constant Expr_Val := Emit_Expr (Ctx, SS_E);
                           begin
                              L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LV.Reg), To_String (LV.Pir_Type), "i64"));
                           end;
                        end if;
                        if Is_Lib_Pool then
                           Add_Global_Decl (Ctx.E, PSym, "extern " & PSym & " = global i64, align 8");
                        else
                           Add_Global_Decl (Ctx.E, PSym, PSym & " = global i64 = 0, align 8");
                        end if;
                        Emit_Inst (Ctx.E, UP & " = addr " & PSym);
                        Emit_Call_Or_Invoke (Ctx, "@__ada_pool_alloc", UP & ", " & To_String (L64) & ", " & Sz_I64, "ptr", Out_Ptr);
                        return;
                     end;
                  end if;
                  Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", Sz_I64, "ptr", Out_Ptr);
               end Emit_Acc_Alloc;
            begin
               if K = DN_SUBTYPE_ALLOCATOR then
                  declare
                     ST : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Node)));
                  begin
                     if not Is_Void_Tree (ST) then
                        Alloc_T := ST;
                        if Ctx.Enable_Checks and then KIND (ST) in DN_CONSTRAINED_ACCESS | DN_CONSTRAINED_RECORD then
                           Emit_Type_Elab_Checks (Ctx, ST);
                        end if;
                     end if;
                  end;
               elsif K = DN_QUALIFIED_ALLOCATOR then
                  declare
                     Q  : constant TREE := D (AS_QUALIFIED, Node);
                     QT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Q)));
                  begin
                     if not Is_Void_Tree (QT) then
                        Alloc_T := QT;
                     end if;
                  end;
               end if;

               -- Access-to-unconstrained-array allocator: heap-allocate data + @__ada_arr_desc!
               if Is_Access_To_Uncon_Array (ET) then
                  declare
                     Base_Arr : constant TREE := Resolve_Type_Spec (Base_Type_Spec (Des_T));
                     BTI      : constant Type_Info := Get_Type_Info (Ctx, Base_Arr);
                     ETI      : constant Type_Info := Get_Type_Info (Ctx, BTI.Elem_Type);
                     EAl      : constant Natural := Elem_Storage_Align (ETI);
                     ESz      : constant Natural := Elem_Storage_Size (ETI);
                     Firsts   : Str_Array_4;
                     Lasts    : Str_Array_4;
                     ND       : Natural := 0;
                     Data_Ptr : Unbounded_String;
                     Desc_Ptr : Unbounded_String;
                  begin
                     if K = DN_QUALIFIED_ALLOCATOR then
                        declare
                           Q      : constant TREE := D (AS_QUALIFIED, Node);
                           -- P3-3.6 & P3-3.15: Evaluate Q (not D(AS_EXP, Q)) so Qual_T bounds/checks run BEFORE Emit_Acc_Alloc!
                           Src    : constant Expr_Val := Emit_Expr (Ctx, Q, Alloc_T);
                           Tot64  : Unbounded_String;
                           Byt    : Unbounded_String;
                           Nz     : constant String := Next_Reg (Ctx.E, "qanz");
                           AlB    : constant String := Next_Reg (Ctx.E, "qaal");
                        begin
                           ND := Natural'Max (1, Natural'Min ((if Src.NDims > 0 then Src.NDims else Natural'Max (1, BTI.NDims)), Max_Array_Dims));
                           for D_Idx in 1 .. ND loop
                              Get_Array_Dim_Bounds (Ctx, Src, D_Idx, Firsts (D_Idx), Lasts (D_Idx));
                           end loop;
                           -- P3-3.15: Check base & designated bounds BEFORE charging storage pool via Emit_Acc_Alloc!
                           if Ctx.Enable_Checks then
                              for D_Idx in 1 .. Natural'Min (ND, BTI.NDims) loop
                                 if not Is_Void_Tree (BTI.Dims (D_Idx).Idx_Type) then
                                    Emit_Non_Null_Index_Check (Ctx, To_String (Firsts (D_Idx)), To_String (Lasts (D_Idx)), BTI.Dims (D_Idx).Idx_Type);
                                 end if;
                              end loop;
                              if not Is_Void_Tree (Des_T) and then KIND (Des_T) = DN_CONSTRAINED_ARRAY then
                                 declare
                                    DesTI  : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                                    DF, DL : Str_Array_4;
                                    ND_D   : constant Natural := Natural'Min (ND, DesTI.NDims);
                                 begin
                                    for D_Idx in 1 .. ND_D loop
                                       Emit_Index_Bounds (Ctx, DesTI.Dims (D_Idx).Idx_Type, DF (D_Idx), DL (D_Idx));
                                    end loop;
                                    Emit_Exact_Bounds_Check (Ctx, Firsts, Lasts, DF, DL, ND_D);
                                 end;
                              end if;
                           end if;
                           Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND, ESz, Tot64, Byt);
                           Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                           Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                           Emit_Acc_Alloc (AlB, Data_Ptr);
                           Emit_Copy_Composite (Ctx, To_String (Data_Ptr), Src, Base_Arr);
                        end;
                     else
                        -- DN_SUBTYPE_ALLOCATOR for array: evaluate bounds from Alloc_T's SM_INDEX_SUBTYPE_S
                        declare
                           Sub_S : constant TREE :=
                             (if not Is_Void_Tree (Alloc_T) and then KIND (Alloc_T) = DN_CONSTRAINED_ARRAY
                              then D (SM_INDEX_SUBTYPE_S, Alloc_T) else CONST_VOID);
                           Tot64 : Unbounded_String;
                           Byt   : Unbounded_String;
                           Nz    : constant String := Next_Reg (Ctx.E, "sanz");
                           AlB   : constant String := Next_Reg (Ctx.E, "saal");
                        begin
                           if not Is_Void_Tree (Sub_S) then
                              declare
                                 IL    : SEQ_TYPE := LIST (Sub_S);
                                 Idx_T : TREE;
                              begin
                                 while not IS_EMPTY (IL) and then ND < Max_Array_Dims loop
                                    POP (IL, Idx_T);
                                    ND := ND + 1;
                                    declare
                                       RT, RL, RH : Unbounded_String;
                                       F64, L64   : Unbounded_String;
                                       Uns_I      : Boolean;
                                    begin
                                       Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (Idx_T)), RT, RL, RH);
                                       Uns_I := Is_Unsigned_Scalar (Idx_T, To_String (RT));
                                       F64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64", Is_Unsigned => Uns_I));
                                       L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64", Is_Unsigned => Uns_I));
                                       -- LRM 4.8(7) & 3.6.1: check non-null index bounds against array index subtype BEFORE allocating/activating!
                                       if Ctx.Enable_Checks and then ND <= BTI.NDims and then not Is_Void_Tree (BTI.Dims (ND).Idx_Type) then
                                          Emit_Non_Null_Index_Check (Ctx, To_String (F64), To_String (L64), BTI.Dims (ND).Idx_Type);
                                       end if;
                                       Firsts (ND) := F64;
                                       Lasts (ND)  := L64;
                                    end;
                                 end loop;
                              end;
                           end if;
                           if ND = 0 then
                              ND := 1;
                              Firsts (1) := To_Unbounded_String ("1");
                              Lasts (1)  := To_Unbounded_String ("0");
                           end if;
                           -- P3-4.7: Check designated subtype bounds BEFORE heap allocation & task creation/activation!
                           if Ctx.Enable_Checks and then not Is_Void_Tree (Des_T) and then KIND (Des_T) = DN_CONSTRAINED_ARRAY then
                              declare
                                 DesTI  : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                                 DF, DL : Str_Array_4;
                                 ND_D   : constant Natural := Natural'Min (ND, DesTI.NDims);
                              begin
                                 for D_Idx in 1 .. ND_D loop
                                    Emit_Index_Bounds (Ctx, DesTI.Dims (D_Idx).Idx_Type, DF (D_Idx), DL (D_Idx));
                                 end loop;
                                 Emit_Exact_Bounds_Check (Ctx, Firsts, Lasts, DF, DL, ND_D);
                              end;
                           end if;
                           Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND, ESz, Tot64, Byt);
                           Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                           Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                           Emit_Acc_Alloc (AlB, Data_Ptr);
                           -- Default-initialize & activate elements for unconstrained array of tasks/records/access/arrays (c85006e!)
                           if Is_Task_Type (BTI.Elem_Type) or else Is_Record_Type (BTI.Elem_Type)
                             or else Is_Access_Type (BTI.Elem_Type)
                             or else ETI.Kind = TK_Constrained_Array
                           then
                              declare
                                 Saved_M : constant Unbounded_String := Ctx.Cur_Master_Reg;
                                 Mst_R   : constant Unbounded_String := Resolve_Alloc_Master;
                                 Mk_R    : Unbounded_String;
                                 L_Fail  : constant String := Next_Label (Ctx.E, "ualcf");
                                 L_Done  : constant String := Next_Label (Ctx.E, "ualcd");
                                 Ex_R    : constant String := Next_Reg (Ctx.E, "ualce");
                                 L_H     : constant String := Next_Label (Ctx.E, "uai_h");
                                 L_B     : constant String := Next_Label (Ctx.E, "uai_b");
                                 L_E     : constant String := Next_Label (Ctx.E, "uai_e");
                                 IR      : constant String := Next_Reg (Ctx.E, "uai");
                                 Cnd     : constant String := Next_Reg (Ctx.E, "uac");
                                 Nx      : constant String := Next_Reg (Ctx.E, "uanx");
                                 Dummy   : Unbounded_String;
                              begin
                                 Ctx.Cur_Master_Reg := Mst_R;
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_master_mark", To_String (Mst_R), "ptr", Mk_R, Nounwind => True);
                                 Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (L_Fail),
                                                       Current_Exn   => Null_Unbounded_String,
                                                       In_Handler    => False,
                                                       SS_Mark       => Null_Unbounded_String,
                                                       SP_Mark       => Null_Unbounded_String));
                                 Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                                 Emit_Label (Ctx.E, L_H, IR & ": i64");
                                 Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                                 Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                                 Emit_Label (Ctx.E, L_B);
                                 declare
                                    EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (Data_Ptr), IR);
                                 begin
                                    Emit_Default_Init (Ctx, EP2, BTI.Elem_Type, Activate_Tasks => False);
                                 end;
                                 Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                                 Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                                 Emit_Label (Ctx.E, L_E);
                                 Ctx.Handlers.Delete_Last;
                                 Ctx.Cur_Master_Reg := Saved_M;
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_since",
                                   To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy);
                                 Emit_Term (Ctx.E, "jmp " & L_Done);
                                 Emit_Label (Ctx.E, L_Fail, Ex_R & ": ptr");
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_since",
                                   To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy, Nounwind => True);
                                 Emit_Raise_Reg (Ctx, Ex_R);
                                 Emit_Label (Ctx.E, L_Done);
                              end;
                           end if;
                        end;
                     end if;
                     Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", Img (Arr_Desc_Size), "ptr", Desc_Ptr);
                     for D_Idx in 1 .. Natural'Min (ND, Max_Array_Dims) loop
                        -- Check non-null bounds belong to Base_Arr's index subtype (c48008c/d, c48009d/e)
                        if Ctx.Enable_Checks and then D_Idx <= BTI.NDims and then not Is_Void_Tree (BTI.Dims (D_Idx).Idx_Type) then
                           Emit_Non_Null_Index_Check (Ctx, To_String (Firsts (D_Idx)), To_String (Lasts (D_Idx)), BTI.Dims (D_Idx).Idx_Type);
                        end if;
                     end loop;
                     Emit_Store_Arr_Desc (Ctx, To_String (Desc_Ptr), To_String (Data_Ptr), Firsts, Lasts, ND);
                     -- Check designated subtype bounds if ET is constrained access-to-array (c48007c, c48008c, c48009d/e/h)
                     if Ctx.Enable_Checks and then not Is_Void_Tree (Des_T) and then KIND (Des_T) = DN_CONSTRAINED_ARRAY then
                        declare
                           DesTI  : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                           DF, DL : Str_Array_4;
                           ND_D   : constant Natural := Natural'Min (ND, DesTI.NDims);
                        begin
                           for D_Idx in 1 .. ND_D loop
                              Emit_Index_Bounds (Ctx, DesTI.Dims (D_Idx).Idx_Type, DF (D_Idx), DL (D_Idx));
                           end loop;
                           Emit_Exact_Bounds_Check (Ctx, Firsts, Lasts, DF, DL, ND_D);
                        end;
                     else
                        Emit_Discriminant_Check (Ctx, To_String (Desc_Ptr), ET, Is_Access => True);
                     end if;
                     Res.Kind     := EK_Scalar;
                     Res.Reg      := Desc_Ptr;
                     Res.Pir_Type := To_Unbounded_String ("ptr");
                     Res.Ada_Type := ET;
                     Res.Align    := 8;
                     return Res;
                  end;
               end if;

               declare
                  DTI : constant Type_Info := Get_Type_Info (Ctx, Alloc_T);
                  -- Helper: check exact bounds of an allocated constrained array against Des_T (c48007c, c48009d/e/h)
                  procedure Check_Alloc_Arr_Des_Bounds (AF, AL : Str_Array_4; ND_A : Natural) is
                  begin
                     if Ctx.Enable_Checks and then not Is_Void_Tree (Des_T) and then KIND (Des_T) = DN_CONSTRAINED_ARRAY then
                        declare
                           DesTI  : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                           DF, DL : Str_Array_4;
                           ND_D   : constant Natural := Natural'Min (ND_A, DesTI.NDims);
                        begin
                           for D_I in 1 .. ND_D loop
                              Emit_Index_Bounds (Ctx, DesTI.Dims (D_I).Idx_Type, DF (D_I), DL (D_I));
                           end loop;
                           Emit_Exact_Bounds_Check (Ctx, AF, AL, DF, DL, ND_D);
                        end;
                     end if;
                  end Check_Alloc_Arr_Des_Bounds;
               begin
                  if (K = DN_SUBTYPE_ALLOCATOR or else K = DN_QUALIFIED_ALLOCATOR)
                    and then DTI.Kind = TK_Constrained_Array
                    and then DTI.Is_Dynamic
                  then
                     -- Dynamic constrained array allocator (`new ARR` or `new ARR'(...)` where ARR has dynamic bounds)
                     declare
                        ETI    : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
                        EAl    : constant Natural := Elem_Storage_Align (ETI);
                        ESz    : constant Natural := Elem_Storage_Size (ETI);
                        ND     : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
                        Firsts : Str_Array_4;
                        Lasts  : Str_Array_4;
                        Tot64  : Unbounded_String;
                        Byt    : Unbounded_String;
                        Nz     : constant String := Next_Reg (Ctx.E, "danz");
                        AlB    : constant String := Next_Reg (Ctx.E, "daal");
                        Saved_M : constant Unbounded_String := Ctx.Cur_Master_Reg;
                        Mst_R  : constant Unbounded_String := Resolve_Alloc_Master;
                        Mk_R   : Unbounded_String;
                        L_Fail : constant String := Next_Label (Ctx.E, "dalcf");
                        L_Done : constant String := Next_Label (Ctx.E, "dalcd");
                        Ex_R   : constant String := Next_Reg (Ctx.E, "dalce");
                        Dummy  : Unbounded_String;
                     begin
                        for D_I in 1 .. ND loop
                           Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                        end loop;
                        Check_Alloc_Arr_Des_Bounds (Firsts, Lasts, ND);
                        Emit_Array_Byte_Size_I64 (Ctx, Firsts, Lasts, ND, ESz, Tot64, Byt);
                        Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (Byt) & ", 0");
                        Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (Byt) & ", " & Img (EAl));
                        if K = DN_QUALIFIED_ALLOCATOR then
                           -- P3-3.15: Initialize & check qualified expr into temp SS buffer BEFORE charging pool via Emit_Acc_Alloc!
                           declare
                              Q      : constant TREE := D (AS_QUALIFIED, Node);
                              Init_E : constant TREE := D (AS_EXP, Q);
                              Tmp_B  : Unbounded_String;
                           begin
                              Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", AlB & ", " & Img (EAl), "ptr", Tmp_B);
                              Emit_Init_Composite (Ctx, To_String (Tmp_B), Alloc_T, Init_E,
                                Dst_Firsts => Firsts, Dst_Lasts => Lasts, Exact_Bounds => True);
                              Emit_Acc_Alloc (AlB, Ptr_R);
                              Emit_Inst (Ctx.E, "memcpy " & To_String (Ptr_R) & ", " & To_String (Tmp_B) & ", " & AlB & ", align " & Img (EAl));
                           end;
                        elsif Is_Task_Type (DTI.Elem_Type) or else Is_Record_Type (DTI.Elem_Type)
                          or else Is_Access_Type (DTI.Elem_Type) or else ETI.Kind = TK_Constrained_Array
                        then
                           Emit_Acc_Alloc (AlB, Ptr_R);
                           Ctx.Cur_Master_Reg := Mst_R;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_mark", To_String (Mst_R), "ptr", Mk_R, Nounwind => True);
                           Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (L_Fail),
                                                 Current_Exn   => Null_Unbounded_String,
                                                 In_Handler    => False,
                                                 SS_Mark       => Null_Unbounded_String,
                                                 SP_Mark       => Null_Unbounded_String));
                           declare
                              L_H : constant String := Next_Label (Ctx.E, "dai_h");
                              L_B : constant String := Next_Label (Ctx.E, "dai_b");
                              L_E : constant String := Next_Label (Ctx.E, "dai_e");
                              IR  : constant String := Next_Reg (Ctx.E, "dai");
                              Cnd : constant String := Next_Reg (Ctx.E, "dac");
                              Nx  : constant String := Next_Reg (Ctx.E, "danx");
                           begin
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              declare
                                 EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (Ptr_R), IR);
                              begin
                                 Emit_Default_Init (Ctx, EP2, DTI.Elem_Type, Activate_Tasks => False);
                              end;
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                           end;
                           Ctx.Handlers.Delete_Last;
                           Ctx.Cur_Master_Reg := Saved_M;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_since",
                             To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy);
                           Emit_Term (Ctx.E, "jmp " & L_Done);
                           Emit_Label (Ctx.E, L_Fail, Ex_R & ": ptr");
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_since",
                             To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy, Nounwind => True);
                           Emit_Raise_Reg (Ctx, Ex_R);
                           Emit_Label (Ctx.E, L_Done);
                        else
                           Emit_Acc_Alloc (AlB, Ptr_R);
                        end if;
                     end;
                  else
                  if DTI.Kind = TK_Constrained_Array then
                     declare
                        ND_S : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, Max_Array_Dims));
                        AF, AL : Str_Array_4;
                     begin
                        for D_I in 1 .. ND_S loop
                           Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, AF (D_I), AL (D_I));
                        end loop;
                        Check_Alloc_Arr_Des_Bounds (AF, AL, ND_S);
                     end;
                  end if;
                  if K = DN_QUALIFIED_ALLOCATOR and then DTI.Kind = TK_Unconstrained_Array then
                     declare
                        Q      : constant TREE := D (AS_QUALIFIED, Node);
                        Init_E : constant TREE := D (AS_EXP, Q);
                        Src    : constant Expr_Val := Emit_Expr (Ctx, Init_E, Alloc_T);
                        ND_U   : constant Natural := Natural'Max (1, Natural'Min ((if Src.NDims > 0 then Src.NDims else DTI.NDims), Max_Array_Dims));
                        AF, AL : Str_Array_4;
                        DesTI  : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                     begin
                        for D_I in 1 .. ND_U loop
                           Get_Array_Dim_Bounds (Ctx, Src, D_I, AF (D_I), AL (D_I));
                        end loop;
                        Check_Alloc_Arr_Des_Bounds (AF, AL, ND_U);
                        if DesTI.Kind = TK_Constrained_Array and then DesTI.Is_Dynamic then
                           declare
                              ETI_D : constant Type_Info := Get_Type_Info (Ctx, DesTI.Elem_Type);
                              EAl_D : constant Natural := Elem_Storage_Align (ETI_D);
                              ESz_D : constant Natural := Elem_Storage_Size (ETI_D);
                              Tot_D, Byt_D : Unbounded_String;
                              Nz_D  : constant String := Next_Reg (Ctx.E, "qan2");
                              AlB_D : constant String := Next_Reg (Ctx.E, "qaa2");
                           begin
                              Emit_Array_Byte_Size_I64 (Ctx, AF, AL, ND_U, ESz_D, Tot_D, Byt_D);
                              Emit_Inst (Ctx.E, Nz_D & " = cmp_gt_s.i64 " & To_String (Byt_D) & ", 0");
                              Emit_Inst (Ctx.E, AlB_D & " = select.i64 " & Nz_D & ", " & To_String (Byt_D) & ", " & Img (EAl_D));
                              Emit_Acc_Alloc (AlB_D, Ptr_R);
                           end;
                        else
                           Emit_Acc_Alloc (Img (Natural'Max (1, DesTI.Byte_Size)), Ptr_R);
                        end if;
                        Emit_Copy_Composite (Ctx, To_String (Ptr_R), Src, Des_T);
                     end;
                  else
                  if K = DN_QUALIFIED_ALLOCATOR then
                     -- P3-3.15: Evaluate qualified expr & check constraints BEFORE charging storage pool via Emit_Acc_Alloc!
                     declare
                        Q : constant TREE := D (AS_QUALIFIED, Node);
                        Init_E : constant TREE := D (AS_EXP, Q);
                     begin
                        if DTI.Kind = TK_Scalar then
                           declare
                              V  : constant Expr_Val := Emit_Expr (Ctx, Init_E, Alloc_T);
                              VP : constant String := To_String (V.Pir_Type);
                              DP : constant String := To_String (DTI.Pir_Type);
                              VR : constant String :=
                                Emit_Typed_Scalar_Convert (Ctx, To_String (V.Reg), V.Ada_Type, VP, Alloc_T, DP, Check_Range => True);
                           begin
                              if Des_T /= Alloc_T then
                                 Emit_Range_Check (Ctx, VR, DP, Des_T);
                              end if;
                              Emit_Discriminant_Check (Ctx, VR, Alloc_T, Is_Access => True);
                              Emit_Discriminant_Check (Ctx, VR, Des_T, Is_Access => True);
                              Emit_Acc_Alloc (Img (Natural'Max (1, DTI.Byte_Size)), Ptr_R);
                              Emit_Inst (Ctx.E,
                                "store." & DP & " " & VR &
                                ", " & To_String (Ptr_R) & ", align " & Img (DTI.Align));
                           end;
                        else
                           declare
                              Sz_B  : constant Natural := Natural'Max (1, DTI.Byte_Size);
                              Al_B  : constant Natural := Natural'Max (1, DTI.Align);
                              Tmp_P : constant String := Next_Reg (Ctx.E, "qat");
                           begin
                              Emit_Entry_Alloca (Ctx.E, Tmp_P & " = alloca [" & Img (Sz_B) & " x i8], align " & Img (Al_B));
                              Emit_Init_Composite (Ctx, Tmp_P, Alloc_T, Init_E, Exact_Bounds => True);
                              Emit_Discriminant_Check (Ctx, Tmp_P, Alloc_T);
                              Emit_Discriminant_Check (Ctx, Tmp_P, Des_T);
                              Emit_Acc_Alloc (Img (Sz_B), Ptr_R);
                              Emit_Inst (Ctx.E, "memcpy " & To_String (Ptr_R) & ", " & Tmp_P & ", " & Img (Sz_B) & ", align " & Img (Al_B));
                           end;
                        end if;
                     end;
                  else
                     Emit_Acc_Alloc (Img (Natural'Max (1, DTI.Byte_Size)), Ptr_R);
                     -- Subtype allocator (`new T`): initialize ALL non-task components first, THEN
                     -- activate newly created tasks; if init raises, abort unactivated tasks (LRM 9.3(4-5))!
                     if Is_Task_Type (Alloc_T) then
                        declare
                           Saved_M : constant Unbounded_String := Ctx.Cur_Master_Reg;
                           TH : Unbounded_String;
                        begin
                           Ctx.Cur_Master_Reg := Resolve_Alloc_Master;
                           TH := To_Unbounded_String (Emit_Create_Task_Handle (Ctx, Alloc_T, Activate_Now => True));
                           Ctx.Cur_Master_Reg := Saved_M;
                           Emit_Inst (Ctx.E, "store.ptr " & To_String (TH) & ", " & To_String (Ptr_R) & ", align 8");
                        end;
                     else
                        declare
                           Saved_M : constant Unbounded_String := Ctx.Cur_Master_Reg;
                           Mst_R  : constant Unbounded_String := Resolve_Alloc_Master;
                           Mk_R   : Unbounded_String;
                           L_Fail : constant String := Next_Label (Ctx.E, "alcf");
                           L_Done : constant String := Next_Label (Ctx.E, "alcd");
                           Ex_R   : constant String := Next_Reg (Ctx.E, "alce");
                           Dummy  : Unbounded_String;
                        begin
                           Ctx.Cur_Master_Reg := Mst_R;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_mark", To_String (Mst_R), "ptr", Mk_R, Nounwind => True);
                           Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (L_Fail),
                                                 Current_Exn   => Null_Unbounded_String,
                                                 In_Handler    => False,
                                                 SS_Mark       => Null_Unbounded_String,
                                                 SP_Mark       => Null_Unbounded_String));
                           Emit_Default_Init (Ctx, To_String (Ptr_R), Alloc_T, Activate_Tasks => False);
                           Emit_Discriminant_Check (Ctx, To_String (Ptr_R), ET, Is_Access => True);
                           Ctx.Handlers.Delete_Last;
                           Ctx.Cur_Master_Reg := Saved_M;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_since",
                             To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy);
                           Emit_Term (Ctx.E, "jmp " & L_Done);
                           Emit_Label (Ctx.E, L_Fail, Ex_R & ": ptr");
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_since",
                             To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy, Nounwind => True);
                           Emit_Raise_Reg (Ctx, Ex_R);
                           Emit_Label (Ctx.E, L_Done);
                        end;
                     end if;
                  end if;
                  end if;
                  end if;
               end;
               Res.Kind     := EK_Scalar;
               Res.Reg      := Ptr_R;
               Res.Pir_Type := To_Unbounded_String ("ptr");
               Res.Ada_Type := ET;
               Res.Align    := 8;
               return Res;
            end;

         when DN_USED_NAME_ID =>
            -- Inside a task body, the task type name denotes the currently executing task (LRM 9.1(7))
            declare
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Node)));
               TS  : TREE := CONST_VOID;
            begin
               if not Is_Void_Tree (Def) then
                  if KIND (Def) in CLASS_TYPE_NAME | DN_TASK_BODY_ID then
                     TS := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def)));
                  elsif KIND (Def) = DN_VARIABLE_ID then
                     TS := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def)));
                  end if;
               end if;
               if Is_Task_Type (TS) then
                  declare
                     Self_R : constant String := Emit_Enclosing_Task_Self_By_Key (Ctx, Tree_Key (TS));
                  begin
                     Res.Kind     := EK_Scalar;
                     Res.Reg      := To_Unbounded_String (Self_R);
                     Res.Pir_Type := To_Unbounded_String ("ptr");
                     Res.Ada_Type := TS;
                     Res.Align    := 8;
                     return Res;
                  end;
               end if;
               declare
                  R : constant String := Next_Reg (Ctx.E, "unk");
               begin
                  Emit_Inst (Ctx.E, R & " = const.i32 0");
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := To_Unbounded_String ("i32"); Res.Align := 4;
                  return Res;
               end;
            end;

         when others =>
            declare
               R : constant String := Next_Reg (Ctx.E, "unk");
            begin
               Emit_Comment (Ctx.E, "Unhandled expr node: " & NODE_IMAGE (K));
               Emit_Inst (Ctx.E, R & " = const.i32 0");
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String ("i32"); Res.Align := 4;
               return Res;
            end;
      end case;
   end Emit_Expr;

   function Emit_Typed_Scalar_Convert
     (Ctx         : in out Context;
      Val_Reg     : String;
      Src_Type    : TREE;
      Src_Pir     : String;
      Dst_Type    : TREE;
      Dst_Pir     : String;
      Check_Range : Boolean := False) return String
   is
      ST : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Src_Type));
      DT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Dst_Type));
      Src_Is_FP : constant Boolean := Src_Pir = "f64" or else Src_Pir = "f32";
   begin
      if not Is_Void_Tree (DT) then
         declare
            Chk  : constant String := (if Ctx.Enable_Checks then "1" else "0");
            UnsD : constant Boolean := Is_Unsigned_Scalar (DT, Dst_Pir);
            MinB : constant Long_Long_Integer := (if UnsD then 0 else Min_Signed_For_Pir (Dst_Pir));
            MaxB : constant Long_Long_Integer :=
              (if UnsD and then Dst_Pir = "i8" then 255
               elsif UnsD and then Dst_Pir = "i16" then 65535
               elsif Dst_Pir = "i8" then 127 elsif Dst_Pir = "i16" then 32767
               elsif Dst_Pir = "i32" then 2147483647 else Long_Long_Integer'Last);
         begin
            if Is_Float_Type (DT) and then (Src_Is_FP or else (not Is_Void_Tree (ST) and then Is_Float_Type (ST))) then
               if Src_Pir = Dst_Pir then
                  if Check_Range then
                     Emit_Range_Check (Ctx, Val_Reg, Dst_Pir, DT);
                  end if;
                  return Val_Reg;
               elsif Src_Pir = "f64" and then Dst_Pir = "f32" then
                  if Ctx.Enable_Checks then
                     Emit_Range_Check (Ctx, Val_Reg, "f64", DT);
                  end if;
                  return Emit_Scalar_Coerce (Ctx, Val_Reg, "f64", "f32");
               else
                  declare
                     R : constant String := Emit_Scalar_Coerce (Ctx, Val_Reg, Src_Pir, Dst_Pir);
                  begin
                     if Check_Range then
                        Emit_Range_Check (Ctx, R, Dst_Pir, DT);
                     end if;
                     return R;
                  end;
               end if;
            elsif Is_Float_Type (DT) and then not Src_Is_FP and then not Is_Void_Tree (ST) and then Is_Fixed_Type (ST) then
               declare
                  Fl : constant String := Next_Reg (Ctx.E, "x2f");
                  Sc : constant String := Next_Reg (Ctx.E, "x2s");
               begin
                  Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & Val_Reg);
                  Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (ST)));
                  if Dst_Pir = "f32" then
                     if Ctx.Enable_Checks then
                        Emit_Range_Check (Ctx, Sc, "f64", DT);
                     end if;
                     return Emit_Scalar_Coerce (Ctx, Sc, "f64", "f32");
                  else
                     if Check_Range then
                        Emit_Range_Check (Ctx, Sc, "f64", DT);
                     end if;
                     return Sc;
                  end if;
               end;
            elsif Is_Fixed_Type (DT) and then (Src_Is_FP or else (not Is_Void_Tree (ST) and then Is_Float_Type (ST))) then
               declare
                  F64 : constant String := Emit_Scalar_Coerce (Ctx, Val_Reg, Src_Pir, "f64");
                  Sc  : constant String := Next_Reg (Ctx.E, "f2x");
                  V64 : Unbounded_String;
                  R   : Unbounded_String;
               begin
                  Emit_Inst (Ctx.E, Sc & " = fdiv.f64 " & F64 & ", " & Format_Float64 (Fixed_Small (DT)));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                    Sc & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                  R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", Dst_Pir));
                  if Check_Range then
                     Emit_Range_Check (Ctx, To_String (R), Dst_Pir, DT);
                  end if;
                  return To_String (R);
               end;
            elsif Is_Fixed_Type (DT) and then not Src_Is_FP and then not Is_Void_Tree (ST) and then Is_Fixed_Type (ST) then
               declare
                  Rat : constant IEEE_Float_64 := Fixed_Small (ST) / Fixed_Small (DT);
               begin
                  if Rat /= 1.0 then
                     declare
                        Fl  : constant String := Next_Reg (Ctx.E, "xxf");
                        Sc  : constant String := Next_Reg (Ctx.E, "xxs");
                        V64 : Unbounded_String;
                        R   : Unbounded_String;
                     begin
                        Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & Val_Reg);
                        Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Rat));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                          Sc & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                        R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", Dst_Pir));
                        if Check_Range then
                           Emit_Range_Check (Ctx, To_String (R), Dst_Pir, DT);
                        end if;
                        return To_String (R);
                     end;
                  end if;
               end;
            elsif Is_Fixed_Type (DT) and then not Src_Is_FP and then not Is_Void_Tree (ST) and then Is_Discrete_Type (ST) then
               declare
                  Fl  : constant String := Next_Reg (Ctx.E, "i2xf");
                  Sc  : constant String := Next_Reg (Ctx.E, "i2xs");
                  Op  : constant String := (if Is_Unsigned_Scalar (ST, Src_Pir) then "uitofp.f64 " else "sitofp.f64 ");
                  V64 : Unbounded_String;
                  R   : Unbounded_String;
               begin
                  Emit_Inst (Ctx.E, Fl & " = " & Op & Val_Reg);
                  Emit_Inst (Ctx.E, Sc & " = fdiv.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (DT)));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                    Sc & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                  R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", Dst_Pir));
                  if Check_Range then
                     Emit_Range_Check (Ctx, To_String (R), Dst_Pir, DT);
                  end if;
                  return To_String (R);
               end;
            elsif Is_Discrete_Type (DT) and then not Src_Is_FP and then not Is_Void_Tree (ST) and then Is_Fixed_Type (ST) then
               declare
                  Fl  : constant String := Next_Reg (Ctx.E, "x2if");
                  Sc  : constant String := Next_Reg (Ctx.E, "x2is");
                  V64 : Unbounded_String;
                  R   : Unbounded_String;
               begin
                  Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & Val_Reg);
                  Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (ST)));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                    Sc & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                  R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", Dst_Pir));
                  if Check_Range then
                     Emit_Range_Check (Ctx, To_String (R), Dst_Pir, DT);
                  end if;
                  return To_String (R);
               end;
            elsif Is_Float_Type (DT) and then not Src_Is_FP and then not Is_Void_Tree (ST) and then Is_Discrete_Type (ST) then
               declare
                  R  : constant String := Next_Reg (Ctx.E, "i2f");
                  Op : constant String := (if Is_Unsigned_Scalar (ST, Src_Pir) then "uitofp." else "sitofp.");
               begin
                  Emit_Inst (Ctx.E, R & " = " & Op & Dst_Pir & " " & Val_Reg);
                  if Check_Range then
                     Emit_Range_Check (Ctx, R, Dst_Pir, DT);
                  end if;
                  return R;
               end;
            elsif Is_Discrete_Type (DT) and then (Src_Is_FP or else (not Is_Void_Tree (ST) and then Is_Float_Type (ST))) then
               -- Ada LRM 4.6(7): round halfway away from zero via C99 round() without double-rounding or silent saturation!
               declare
                  F64 : constant String := Emit_Scalar_Coerce (Ctx, Val_Reg, Src_Pir, "f64");
                  V64 : Unbounded_String;
                  R   : Unbounded_String;
               begin
                  Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                    F64 & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                  R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", Dst_Pir));
                  if Check_Range then
                     Emit_Range_Check (Ctx, To_String (R), Dst_Pir, DT);
                  end if;
                  return To_String (R);
               end;
            end if;
         end;
      end if;

      -- Discrete / same-small-fixed / pointer scalar path
      declare
         SBits : constant Natural := Int_Bits_Of_Pir (Src_Pir);
         DBits : constant Natural := Int_Bits_Of_Pir (Dst_Pir);
         Src_U : constant Boolean := Is_Unsigned_Scalar (ST, Src_Pir);
         Dst_U : constant Boolean := Is_Unsigned_Scalar (DT, Dst_Pir);
         Checked_Pre : Boolean := False;
      begin
         if Ctx.Enable_Checks and then not Is_Void_Tree (DT) then
            if Src_U /= Dst_U and then SBits > 0 and then SBits < 64 and then DBits > 0
              and then (Is_Discrete_Type (DT) or else Is_Fixed_Type (DT))
            then
               declare
                  W64 : constant String := Emit_Int_Convert (Ctx, Val_Reg, Src_Pir, "i64", Is_Unsigned => Src_U);
               begin
                  Emit_Range_Check (Ctx, W64, "i64", DT);
                  return Emit_Int_Convert (Ctx, W64, "i64", Dst_Pir);
               end;
            elsif SBits > DBits and then DBits > 0
              and then (Is_Discrete_Type (DT) or else Is_Fixed_Type (DT))
            then
               Emit_Range_Check (Ctx, Val_Reg, Src_Pir, DT);
               Checked_Pre := True;
            elsif Src_Pir = "f64" and then Dst_Pir = "f32" and then Is_Float_Type (DT) then
               Emit_Range_Check (Ctx, Val_Reg, "f64", DT);
               Checked_Pre := True;
            end if;
         end if;
         declare
            Res_R : constant String := Emit_Scalar_Coerce (Ctx, Val_Reg, Src_Pir, Dst_Pir, Is_Unsigned => Src_U);
         begin
            if Check_Range and then not Checked_Pre and then not Is_Void_Tree (DT) then
               Emit_Range_Check (Ctx, Res_R, Dst_Pir, DT);
            end if;
            return Res_R;
         end;
      end;
   end Emit_Typed_Scalar_Convert;

   procedure Emit_Type_Elab_Checks
     (Ctx                 : in out Context;
      T                   : TREE;
      Depth               : Natural := 0;
      Save_Allowed        : Boolean := True;
      Mark_Type           : TREE := CONST_VOID;
      Has_Real_Constraint : Boolean := False)
   is
      R : constant TREE := Subst_Resolve_Type (Ctx, T);
   begin
      if not Ctx.Enable_Checks or else Is_Void_Tree (R) or else Depth > 8 then
         return;
      end if;
      case KIND (R) is
         when DN_CONSTRAINED_RECORD =>
            declare
               BT : constant TREE := Resolve_Type_Spec (Base_Type_Spec (R));
               DS : constant TREE :=
                 (if not Is_Void_Tree (BT)
                    and then (KIND (BT) = DN_RECORD or else KIND (BT) = DN_PRIVATE or else KIND (BT) = DN_L_PRIVATE)
                  then D (SM_DISCRIMINANT_S, BT) else CONST_VOID);
               CS          : constant TREE := D (SM_NORMALIZED_DSCRMT_S, R);
               Bound_Discs : Temp_Var_Scope;
               SD          : Saved_Disc_Info;
               D_Idx : Natural := 0;
            begin
               if not Is_Void_Tree (DS) and then not Is_Void_Tree (CS) then
                  declare
                     DL : SEQ_TYPE := LIST (DS);
                     CL : SEQ_TYPE := LIST (CS);
                     DItem, CItem : TREE;
                  begin
                     while not IS_EMPTY (DL) and then not IS_EMPTY (CL) loop
                        POP (DL, DItem);
                        if KIND (DItem) = DN_DSCRMT_DECL then
                           declare
                              NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DItem));
                              DID : TREE;
                           begin
                              while not IS_EMPTY (NS) and then not IS_EMPTY (CL) loop
                                 POP (NS, DID);
                                 POP (CL, CItem);
                                 D_Idx := D_Idx + 1;
                                 declare
                                    DT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                    DTI : constant Type_Info := Get_Type_Info (Ctx, DT);
                                    EV  : constant Expr_Val := Emit_Expr (Ctx, CItem, DT);
                                    VR  : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), To_String (DTI.Pir_Type));
                                    DK  : constant Unsigned_32 := Tree_Key (Canonical_Def (DID));
                                    DV  : Var_Info;
                                    SP  : Unbounded_String;
                                    SG  : Boolean;
                                 begin
                                    Emit_Range_Check (Ctx, VR, To_String (DTI.Pir_Type), DT);
                                    if D_Idx <= 8 and then Save_Allowed then
                                       Save_Scalar_Slot (Ctx, VR, To_String (DTI.Pir_Type), DTI.Align, SP, SG);
                                       SD.Count := D_Idx;
                                       SD.Ptrs (D_Idx) := SP;
                                       SD.Pir_Types (D_Idx) := DTI.Pir_Type;
                                       SD.Aligns (D_Idx) := DTI.Align;
                                       SD.Is_Global := SG;
                                       SD.Owner_Subp := Ctx.Cur_Subp_Key;
                                    end if;
                                    DV.Kind       := VK_Param_Val;
                                    DV.Pir_Name   := To_Unbounded_String (VR);
                                    DV.Pir_Type   := DTI.Pir_Type;
                                    DV.Ada_Type   := DT;
                                    DV.Align      := DTI.Align;
                                    DV.Owner_Subp := Ctx.Cur_Subp_Key;
                                    Bind_Temp_Var (Ctx, Bound_Discs, DK, DV);
                                 end;
                              end loop;
                           end;
                        end if;
                     end loop;
                  end;
                  if SD.Count > 0 and then Save_Allowed then
                     Ctx.Saved_Type_Discs.Include (Tree_Key (R), SD);
                  end if;
                  -- LRM 3.7.2(7) (c37215*, c37216a, c37217*): validate fixed & active variant component constraints!
                  if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
                     declare
                        procedure Check_Active_CL (CList : TREE) is
                           VP : TREE;
                        begin
                           if Is_Void_Tree (CList) or else KIND (CList) /= DN_COMP_LIST then return; end if;
                           if not Is_Void_Tree (D (AS_DECL_S, CList)) then
                              declare
                                 DL2 : SEQ_TYPE := LIST (D (AS_DECL_S, CList));
                                 DI  : TREE;
                              begin
                                 while not IS_EMPTY (DL2) loop
                                    POP (DL2, DI);
                                    if KIND (DI) = DN_VARIABLE_DECL then
                                       declare
                                          NS2 : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DI));
                                          CID : TREE;
                                       begin
                                          while not IS_EMPTY (NS2) loop
                                             POP (NS2, CID);
                                             Emit_Type_Elab_Checks (Ctx, D (SM_OBJ_TYPE, CID), Depth + 1, Save_Allowed => False);
                                          end loop;
                                       end;
                                    end if;
                                 end loop;
                              end;
                           end if;
                           VP := D (AS_VARIANT_PART, CList);
                           if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART
                             and then not Is_Void_Tree (D (AS_VARIANT_S, VP))
                           then
                              declare
                                 D_Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, VP))));
                                 DK    : constant Unsigned_32 := Tree_Key (D_Def);
                              begin
                                 if Ctx.Vars.Contains (DK) then
                                    declare
                                       DV_Info : constant Var_Info := Ctx.Vars.Element (DK);
                                       DV64 : constant String := Emit_Int_Convert (Ctx, To_String (DV_Info.Pir_Name), To_String (DV_Info.Pir_Type), "i64",
                                         Is_Unsigned => Is_Unsigned_Scalar (DV_Info.Ada_Type, To_String (DV_Info.Pir_Type)));
                                       VL   : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                                       VN   : TREE;
                                       L_VEnd : constant String := Next_Label (Ctx.E, "tvpe");
                                    begin
                                       while not IS_EMPTY (VL) loop
                                          POP (VL, VN);
                                          if KIND (VN) = DN_VARIANT then
                                             declare
                                                CS2 : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN));
                                                Ch  : TREE;
                                                Hit : Unbounded_String;
                                                Has_Oth : Boolean := False;
                                                L_Hit : constant String := Next_Label (Ctx.E, "tvh");
                                                L_Nxt : constant String := Next_Label (Ctx.E, "tvn");
                                             begin
                                                while not IS_EMPTY (CS2) loop
                                                   POP (CS2, Ch);
                                                   if KIND (Ch) = DN_CHOICE_OTHERS then
                                                      Has_Oth := True;
                                                   elsif KIND (Ch) = DN_CHOICE_EXP then
                                                      declare
                                                         CV  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch), DV_Info.Ada_Type);
                                                         C64 : constant String := Emit_Int_Convert (Ctx, To_String (CV.Reg), To_String (CV.Pir_Type), "i64",
                                                           Is_Unsigned => Is_Unsigned_Scalar (DV_Info.Ada_Type, To_String (CV.Pir_Type)));
                                                         Eq  : constant String := Next_Reg (Ctx.E, "tvce");
                                                      begin
                                                         Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & DV64 & ", " & C64);
                                                         if Length (Hit) = 0 then Hit := To_Unbounded_String (Eq);
                                                         else
                                                            declare
                                                               O : constant String := Next_Reg (Ctx.E, "tvco");
                                                            begin
                                                               Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & Eq);
                                                               Hit := To_Unbounded_String (O);
                                                            end;
                                                         end if;
                                                      end;
                                                   elsif KIND (Ch) = DN_CHOICE_RANGE then
                                                      declare
                                                         RT2, LoR, HiR : Unbounded_String;
                                                         Lo64, Hi64    : Unbounded_String;
                                                         Ge  : constant String := Next_Reg (Ctx.E, "tvge");
                                                         Le  : constant String := Next_Reg (Ctx.E, "tvle");
                                                         InR : constant String := Next_Reg (Ctx.E, "tvin");
                                                      begin
                                                         Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT2, LoR, HiR);
                                                         Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LoR), To_String (RT2), "i64",
                                                           Is_Unsigned => Is_Unsigned_Scalar (DV_Info.Ada_Type, To_String (RT2))));
                                                         Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (HiR), To_String (RT2), "i64",
                                                           Is_Unsigned => Is_Unsigned_Scalar (DV_Info.Ada_Type, To_String (RT2))));
                                                         Emit_Inst (Ctx.E, Ge & " = cmp_ge_s.i64 " & DV64 & ", " & To_String (Lo64));
                                                         Emit_Inst (Ctx.E, Le & " = cmp_le_s.i64 " & DV64 & ", " & To_String (Hi64));
                                                         Emit_Inst (Ctx.E, InR & " = and.i8 " & Ge & ", " & Le);
                                                         if Length (Hit) = 0 then Hit := To_Unbounded_String (InR);
                                                         else
                                                            declare
                                                               O : constant String := Next_Reg (Ctx.E, "tvco");
                                                            begin
                                                               Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & InR);
                                                               Hit := To_Unbounded_String (O);
                                                            end;
                                                         end if;
                                                      end;
                                                   end if;
                                                end loop;
                                                if Has_Oth then
                                                   Check_Active_CL (D (AS_COMP_LIST, VN));
                                                   Emit_Term (Ctx.E, "jmp " & L_VEnd);
                                                   Emit_Label (Ctx.E, L_Nxt);
                                                elsif Length (Hit) > 0 then
                                                   Emit_Term (Ctx.E, "br " & To_String (Hit) & ", " & L_Hit & ", " & L_Nxt);
                                                   Emit_Label (Ctx.E, L_Hit);
                                                   Check_Active_CL (D (AS_COMP_LIST, VN));
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
                        end Check_Active_CL;
                     begin
                        Check_Active_CL (D (SM_COMP_LIST, BT));
                     end;
                  end if;
               end if;
               Restore_Temp_Vars (Ctx, Bound_Discs);
            end;
         when DN_ARRAY | DN_CONSTRAINED_ARRAY =>
            declare
               BT : constant TREE := Resolve_Type_Spec (Base_Type_Spec (R));
            begin
               if KIND (R) = DN_CONSTRAINED_ARRAY and then not Is_Void_Tree (BT) and then KIND (BT) = DN_ARRAY then
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, R);
                     BTI : constant Type_Info := Get_Type_Info (Ctx, BT);
                     ND  : constant Natural := Natural'Min (RTI.NDims, BTI.NDims);
                  begin
                     for D_I in 1 .. ND loop
                        if not Is_Void_Tree (RTI.Dims (D_I).Idx_Type) and then not Is_Void_Tree (BTI.Dims (D_I).Idx_Type) then
                           declare
                              F64, L64 : Unbounded_String;
                              BIT      : constant TREE := BTI.Dims (D_I).Idx_Type;
                           begin
                              Emit_Index_Bounds (Ctx, RTI.Dims (D_I).Idx_Type, F64, L64);
                              if Save_Allowed and then not RTI.Dims (D_I).Is_Static then
                                 declare
                                    IT_K : constant Unsigned_32 := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, RTI.Dims (D_I).Idx_Type)));
                                    SR   : Saved_Range_Info;
                                 begin
                                    Save_Scalar_Slot (Ctx, To_String (F64), "i64", 8, SR.Lo_Ptr, SR.Is_Global);
                                    Save_Scalar_Slot (Ctx, To_String (L64), "i64", 8, SR.Hi_Ptr, SR.Is_Global);
                                    SR.Pir_Type   := To_Unbounded_String ("i64");
                                    SR.Align      := 8;
                                    SR.Owner_Subp := Ctx.Cur_Subp_Key;
                                    Ctx.Saved_Type_Ranges.Include (IT_K, SR);
                                 end;
                              end if;
                              Emit_Non_Null_Index_Check (Ctx, To_String (F64), To_String (L64), BIT);
                           end;
                        end if;
                     end loop;
                  end;
               end if;
               if not Is_Void_Tree (BT) and then KIND (BT) = DN_ARRAY then
                  Emit_Type_Elab_Checks (Ctx, D (SM_COMP_TYPE, BT), Depth + 1);
               end if;
            end;
         when DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED =>
            declare
               BT  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (R));
               Der : constant TREE := D (SM_DERIVED, R);
               MT_R : constant TREE :=
                 (if not Is_Void_Tree (Mark_Type) then Subst_Resolve_Type (Ctx, Mark_Type)
                  else CONST_VOID);
               Parent_T : constant TREE :=
                 (if not Is_Void_Tree (MT_R) and then KIND (MT_R) in DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED
                  then MT_R
                  elsif not Is_Void_Tree (Der) and then KIND (Resolve_Type_Spec (Der)) in DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED
                  then Resolve_Type_Spec (Der)
                  else BT);
               Rng : constant TREE := D (SM_RANGE, R);
            begin
               -- LRM 3.5.7(11) & 3.5.9(10) (c35711a/b, c35904a/b): float DIGITS / fixed DELTA compatibility vs parent subtype
               if Has_Real_Constraint and then not Is_Void_Tree (MT_R) and then not Is_Void_Tree (Parent_T) and then Parent_T /= R then
                  if KIND (R) = DN_FLOAT and then KIND (Parent_T) = DN_FLOAT
                    and then not Is_Void_Tree (D (SM_ACCURACY, R))
                    and then not Is_Void_Tree (D (SM_ACCURACY, Parent_T))
                    and then Has_Static_Val (D (SM_ACCURACY, R))
                    and then Has_Static_Val (D (SM_ACCURACY, Parent_T))
                    and then Eval_Int64 (D (SM_ACCURACY, R)) > Eval_Int64 (D (SM_ACCURACY, Parent_T))
                  then
                     Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                     declare
                        Unr : constant String := Next_Label (Ctx.E, "tdg_u");
                     begin
                        Emit_Label (Ctx.E, Unr);
                     end;
                  elsif KIND (R) = DN_FIXED and then KIND (Parent_T) = DN_FIXED
                    and then Fixed_Delta (R) > 0.0 and then Fixed_Delta (Parent_T) > 0.0
                    and then Fixed_Delta (R) < Fixed_Delta (Parent_T)
                  then
                     Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                     declare
                        Unr : constant String := Next_Label (Ctx.E, "tdl_u");
                     begin
                        Emit_Label (Ctx.E, Unr);
                     end;
                  end if;
               end if;
               -- Real/discrete range compatibility vs explicit parent subtype mark (c35003d/f, c35711a/b, c35904a/b)
               declare
                  Real_Checked_Static : Boolean := False;
               begin
               if not Is_Void_Tree (MT_R) and then not Is_Void_Tree (Parent_T) and then Parent_T /= R
                 and then KIND (R) in DN_FLOAT | DN_FIXED
                 and then not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE
                 and then Rng /= D (SM_RANGE, Parent_T)
                 and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, Rng)))
                 and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, Rng)))
               then
                  declare
                     PRng : constant TREE := D (SM_RANGE, Parent_T);
                  begin
                     if not Is_Void_Tree (PRng) and then KIND (PRng) = DN_RANGE
                       and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, PRng)))
                       and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, PRng)))
                     then
                        Real_Checked_Static := True;
                        declare
                           Lo : constant IEEE_Float_64 := Eval_Float64 (Safe_SM_Value (D (AS_EXP1, Rng)));
                           Hi : constant IEEE_Float_64 := Eval_Float64 (Safe_SM_Value (D (AS_EXP2, Rng)));
                           PLo : constant IEEE_Float_64 := Eval_Float64 (Safe_SM_Value (D (AS_EXP1, PRng)));
                           PHi : constant IEEE_Float_64 := Eval_Float64 (Safe_SM_Value (D (AS_EXP2, PRng)));
                        begin
                           if Lo <= Hi and then (Lo < PLo or else Hi > PHi) then
                              Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                              declare
                                 Unr : constant String := Next_Label (Ctx.E, "trf_u");
                              begin
                                 Emit_Label (Ctx.E, Unr);
                              end;
                           end if;
                        end;
                     end if;
                  end;
               end if;
               -- DN_RANGE_ATTRIBUTE on scalar subtype (c35003a/b, c36204b/c, c36305a)
               if not Is_Void_Tree (Parent_T) and then Parent_T /= R
                 and then KIND (R) in DN_INTEGER | DN_ENUMERATION
                 and then not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE_ATTRIBUTE
               then
                  declare
                     PTI  : constant Type_Info := Get_Type_Info (Ctx, Parent_T);
                     PT   : constant String := To_String (PTI.Pir_Type);
                     RT_U, Lo_U, Hi_U : Unbounded_String;
                     NE   : constant String := Next_Reg (Ctx.E, "trne");
                     L_C  : constant String := Next_Label (Ctx.E, "trc");
                     L_O  : constant String := Next_Label (Ctx.E, "tro");
                  begin
                     Emit_Discrete_Range (Ctx, Rng, RT_U, Lo_U, Hi_U);
                     declare
                        R1 : constant String := Emit_Int_Convert (Ctx, To_String (Lo_U), To_String (RT_U), PT,
                          Is_Unsigned => Is_Unsigned_Scalar (Parent_T, To_String (RT_U)));
                        R2 : constant String := Emit_Int_Convert (Ctx, To_String (Hi_U), To_String (RT_U), PT,
                          Is_Unsigned => Is_Unsigned_Scalar (Parent_T, To_String (RT_U)));
                        Le_Op : constant String := (if Is_Unsigned_Scalar (Parent_T, PT) then "cmp_le_u." else "cmp_le_s.");
                     begin
                        Emit_Inst (Ctx.E, NE & " = " & Le_Op & PT & " " & R1 & ", " & R2);
                        Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                        Emit_Label (Ctx.E, L_C);
                        Emit_Range_Check (Ctx, R1, PT, Parent_T);
                        Emit_Range_Check (Ctx, R2, PT, Parent_T);
                        Emit_Term (Ctx.E, "jmp " & L_O);
                        Emit_Label (Ctx.E, L_O);
                        if Save_Allowed then
                           declare
                              SR : Saved_Range_Info;
                           begin
                              Save_Scalar_Slot (Ctx, R1, PT, Natural'Max (1, PTI.Align), SR.Lo_Ptr, SR.Is_Global);
                              Save_Scalar_Slot (Ctx, R2, PT, Natural'Max (1, PTI.Align), SR.Hi_Ptr, SR.Is_Global);
                              SR.Pir_Type   := PTI.Pir_Type;
                              SR.Align      := Natural'Max (1, PTI.Align);
                              SR.Owner_Subp := Ctx.Cur_Subp_Key;
                              Ctx.Saved_Type_Ranges.Include (Tree_Key (R), SR);
                           end;
                        end if;
                     end;
                  end;
               elsif not Is_Void_Tree (Parent_T) and then Parent_T /= R
                 and then not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE
                 and then Rng /= D (SM_RANGE, Parent_T)
                 and then (KIND (R) in DN_INTEGER | DN_ENUMERATION or else not Real_Checked_Static)
               then
                  declare
                     PTI : constant Type_Info := Get_Type_Info (Ctx, Parent_T);
                     E1  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng), Parent_T);
                     E2  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng), Parent_T);
                     PT  : constant String := To_String (PTI.Pir_Type);
                     R1  : Unbounded_String;
                     R2  : Unbounded_String;
                     NE  : constant String := Next_Reg (Ctx.E, "tsne");
                     L_C : constant String := Next_Label (Ctx.E, "tsc");
                     L_O : constant String := Next_Label (Ctx.E, "tso");
                  begin
                     if KIND (R) = DN_FIXED then
                        declare
                           SV1 : constant TREE := Safe_SM_Value (D (AS_EXP1, Rng));
                           SV2 : constant TREE := Safe_SM_Value (D (AS_EXP2, Rng));
                           TV  : Integer_64;
                           Ovf : Boolean;
                        begin
                           if Has_Static_Val (SV1) then
                              Eval_Fixed_Ticks_Exact_Checked (SV1, R, TV, Ovf);
                              if Ovf and then Ctx.Enable_Checks then
                                 declare
                                    B1 : constant String := Next_Reg (Ctx.E, "fxo1");
                                 begin
                                    Emit_Inst (Ctx.E, B1 & " = const.i8 1");
                                    Emit_Check_Fail_If (Ctx, B1);
                                 end;
                                 return;
                              end if;
                              declare
                                 CR : constant String := Next_Reg (Ctx.E, "fxb1");
                              begin
                                 Emit_Inst (Ctx.E, CR & " = const." & PT & " " & Img (Long_Long_Integer (TV)));
                                 R1 := To_Unbounded_String (CR);
                              end;
                           elsif To_String (E1.Pir_Type) = "f32" or else To_String (E1.Pir_Type) = "f64" then
                              R1 := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), R, PT));
                           else
                              R1 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), PT));
                           end if;
                           if Has_Static_Val (SV2) then
                              Eval_Fixed_Ticks_Exact_Checked (SV2, R, TV, Ovf);
                              if Ovf and then Ctx.Enable_Checks then
                                 declare
                                    B2 : constant String := Next_Reg (Ctx.E, "fxo2");
                                 begin
                                    Emit_Inst (Ctx.E, B2 & " = const.i8 1");
                                    Emit_Check_Fail_If (Ctx, B2);
                                 end;
                                 return;
                              end if;
                              declare
                                 CR : constant String := Next_Reg (Ctx.E, "fxb2");
                              begin
                                 Emit_Inst (Ctx.E, CR & " = const." & PT & " " & Img (Long_Long_Integer (TV)));
                                 R2 := To_Unbounded_String (CR);
                              end;
                           elsif To_String (E2.Pir_Type) = "f32" or else To_String (E2.Pir_Type) = "f64" then
                              R2 := To_Unbounded_String (Emit_F64_To_Fixed_Ticks (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), R, PT));
                           else
                              R2 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), PT));
                           end if;
                        end;
                     else
                        R1 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), PT,
                                Is_Unsigned => Is_Unsigned_Scalar (E1.Ada_Type, To_String (E1.Pir_Type))));
                        R2 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), PT,
                                Is_Unsigned => Is_Unsigned_Scalar (E2.Ada_Type, To_String (E2.Pir_Type))));
                     end if;
                     if PT = "f32" or else PT = "f64" then
                        Emit_Inst (Ctx.E, NE & " = fcmp_ole." & PT & " " & To_String (R1) & ", " & To_String (R2));
                     elsif Is_Unsigned_Scalar (Parent_T, PT) then
                        Emit_Inst (Ctx.E, NE & " = cmp_le_u." & PT & " " & To_String (R1) & ", " & To_String (R2));
                     else
                        Emit_Inst (Ctx.E, NE & " = cmp_le_s." & PT & " " & To_String (R1) & ", " & To_String (R2));
                     end if;
                     Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                     Emit_Label (Ctx.E, L_C);
                     Emit_Range_Check (Ctx, To_String (R1), PT, Parent_T);
                     Emit_Range_Check (Ctx, To_String (R2), PT, Parent_T);
                     Emit_Term (Ctx.E, "jmp " & L_O);
                     Emit_Label (Ctx.E, L_O);
                     -- Save pre-evaluated bounds so subsequent range checks do NOT re-evaluate side effects (c32001a, c32107a)
                     if Save_Allowed then
                        declare
                           SR : Saved_Range_Info;
                        begin
                           Save_Scalar_Slot (Ctx, To_String (R1), PT, PTI.Align, SR.Lo_Ptr, SR.Is_Global);
                           Save_Scalar_Slot (Ctx, To_String (R2), PT, PTI.Align, SR.Hi_Ptr, SR.Is_Global);
                           SR.Pir_Type   := PTI.Pir_Type;
                           SR.Align      := PTI.Align;
                           SR.Owner_Subp := Ctx.Cur_Subp_Key;
                           Ctx.Saved_Type_Ranges.Include (Tree_Key (R), SR);
                        end;
                     end if;
                  end;
               end if;
               end;
            end;
         when DN_ACCESS | DN_CONSTRAINED_ACCESS =>
            Emit_Type_Elab_Checks (Ctx, D (SM_DESIG_TYPE, R), Depth + 1, Save_Allowed => Save_Allowed,
              Mark_Type => Mark_Type, Has_Real_Constraint => Has_Real_Constraint);
         when DN_RECORD =>
            declare
               DS : constant TREE := D (SM_DISCRIMINANT_S, R);
               CL : constant TREE := D (SM_COMP_LIST, R);
               Has_Discs : constant Boolean := not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS));

               procedure Pre_Eval_Expr (E : TREE; Tgt_T : TREE) is
               begin
                  if Is_Void_Tree (E) or else Has_Static_Val (Safe_SM_Value (E))
                    or else Expr_Has_Unbound_Discr (Ctx, E)
                    or else Ctx.Saved_Expr_Vals.Contains (Tree_Key (E))
                  then
                     return;
                  end if;
                  declare
                     EV : constant Expr_Val := Emit_Expr (Ctx, E, Tgt_T);
                     SE : Saved_Expr_Info;
                  begin
                     if EV.Kind = EK_Scalar then
                        Save_Scalar_Slot (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), EV.Align, SE.Ptr_Reg, SE.Is_Global);
                        SE.Pir_Type   := EV.Pir_Type;
                        SE.Ada_Type   := EV.Ada_Type;
                        SE.Align      := EV.Align;
                        SE.Owner_Subp := Ctx.Cur_Subp_Key;
                        Ctx.Saved_Expr_Vals.Include (Tree_Key (E), SE);
                     end if;
                  end;
               end Pre_Eval_Expr;

               procedure Pre_Eval_Comp_Type (CT_Raw : TREE; Comp_Mark : TREE := CONST_VOID) is
                  CT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, CT_Raw));
               begin
                  if Is_Void_Tree (CT) then return; end if;
                  if KIND (CT) = DN_CONSTRAINED_RECORD then
                     declare
                        CS : constant TREE := D (SM_NORMALIZED_DSCRMT_S, CT);
                     begin
                        if not Is_Void_Tree (CS) then
                           if not Expr_Has_Unbound_Discr (Ctx, CS) then
                              Emit_Type_Elab_Checks (Ctx, CT, Depth + 1, Mark_Type => Comp_Mark);
                           else
                              -- LRM 3.7.2(5) (c37213a..d): evaluate non-discriminant exprs NOW, defer range check until record is constrained!
                              declare
                                 CL2 : SEQ_TYPE := LIST (CS);
                                 CE  : TREE;
                              begin
                                 while not IS_EMPTY (CL2) loop
                                    POP (CL2, CE);
                                    Pre_Eval_Expr (CE, CONST_VOID);
                                 end loop;
                              end;
                           end if;
                        end if;
                     end;
                  elsif KIND (CT) = DN_CONSTRAINED_ARRAY then
                     declare
                        IS_N : constant TREE := D (SM_INDEX_SUBTYPE_S, CT);
                        Has_D : Boolean := False;
                     begin
                        if not Is_Void_Tree (IS_N) then
                           declare
                              IL : SEQ_TYPE := LIST (IS_N);
                              IT : TREE;
                           begin
                              while not IS_EMPTY (IL) loop
                                 POP (IL, IT);
                                 if Expr_Has_Unbound_Discr (Ctx, IT) then
                                    Has_D := True;
                                 end if;
                              end loop;
                           end;
                           if not Has_D then
                              Emit_Type_Elab_Checks (Ctx, CT, Depth + 1, Mark_Type => Comp_Mark);
                           else
                              declare
                                 IL : SEQ_TYPE := LIST (IS_N);
                                 IT : TREE;
                              begin
                                 while not IS_EMPTY (IL) loop
                                    POP (IL, IT);
                                    declare
                                       RI  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, IT));
                                       Rng : constant TREE := (if not Is_Void_Tree (RI) and then KIND (RI) in DN_INTEGER | DN_ENUMERATION then D (SM_RANGE, RI) else CONST_VOID);
                                    begin
                                       if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                                          Pre_Eval_Expr (D (AS_EXP1, Rng), CONST_VOID);
                                          Pre_Eval_Expr (D (AS_EXP2, Rng), CONST_VOID);
                                       end if;
                                    end;
                                 end loop;
                              end;
                           end if;
                        end if;
                     end;
                  elsif KIND (CT) = DN_CONSTRAINED_ACCESS then
                     Pre_Eval_Comp_Type (D (SM_DESIG_TYPE, CT), Comp_Mark);
                  elsif not Has_Discs or else KIND (CT) in DN_INTEGER | DN_ENUMERATION | DN_FLOAT | DN_FIXED then
                     Emit_Type_Elab_Checks (Ctx, CT, Depth + 1, Mark_Type => Comp_Mark);
                  end if;
               end Pre_Eval_Comp_Type;

               procedure Walk_CL (CList : TREE) is
                  VP : TREE;
               begin
                  if Is_Void_Tree (CList) or else KIND (CList) /= DN_COMP_LIST then return; end if;
                  if not Is_Void_Tree (D (AS_DECL_S, CList)) then
                     declare
                        DL : SEQ_TYPE := LIST (D (AS_DECL_S, CList));
                        It : TREE;
                     begin
                        while not IS_EMPTY (DL) loop
                           POP (DL, It);
                           if KIND (It) = DN_VARIABLE_DECL then
                              declare
                                 NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, It));
                                 CID : TREE;
                                 TD_C : constant TREE := D (AS_TYPE_DEF, It);
                                 CMark : constant TREE :=
                                   (if not Is_Void_Tree (TD_C) and then KIND (TD_C) = DN_SUBTYPE_INDICATION
                                      and then not Is_Void_Tree (D (AS_NAME, TD_C))
                                      and then not Is_Void_Tree (Get_Name_Defn (D (AS_NAME, TD_C)))
                                      and then KIND (Get_Name_Defn (D (AS_NAME, TD_C))) in CLASS_TYPE_NAME
                                    then D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, TD_C)))
                                    else CONST_VOID);
                              begin
                                 while not IS_EMPTY (NS) loop
                                    POP (NS, CID);
                                    declare
                                       CT_W  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID)));
                                       CID_K : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                    begin
                                       if not Is_Void_Tree (CT_W) and then CT_W /= Resolve_Type_Spec (Subst_Tree (Ctx, CMark)) then
                                          Ctx.Saved_Type_Ranges.Exclude (Tree_Key (CT_W));
                                          Ctx.Saved_Type_Discs.Exclude (Tree_Key (CT_W));
                                          if KIND (CT_W) = DN_CONSTRAINED_ARRAY then
                                             declare
                                                TI_W : constant Type_Info := Get_Type_Info (Ctx, CT_W);
                                             begin
                                                for D_I in 1 .. TI_W.NDims loop
                                                   if not Is_Void_Tree (TI_W.Dims (D_I).Idx_Type) then
                                                      Ctx.Saved_Type_Ranges.Exclude (Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, TI_W.Dims (D_I).Idx_Type))));
                                                   end if;
                                                end loop;
                                             end;
                                          end if;
                                       end if;
                                       Pre_Eval_Comp_Type (D (SM_OBJ_TYPE, CID), CMark);
                                       -- Save per-component bounds/discriminants under CID_K so multi-component decls (c37003a) keep distinct slots!
                                       if not Is_Void_Tree (CT_W) then
                                          if Ctx.Saved_Type_Discs.Contains (Tree_Key (CT_W)) then
                                             Ctx.Saved_Type_Discs.Include (CID_K, Ctx.Saved_Type_Discs.Element (Tree_Key (CT_W)));
                                          elsif Ctx.Saved_Type_Ranges.Contains (Tree_Key (CT_W)) then
                                             Ctx.Saved_Type_Ranges.Include (CID_K, Ctx.Saved_Type_Ranges.Element (Tree_Key (CT_W)));
                                          elsif KIND (CT_W) = DN_CONSTRAINED_ARRAY then
                                             declare
                                                TI_W : constant Type_Info := Get_Type_Info (Ctx, CT_W);
                                             begin
                                                if TI_W.NDims >= 1 and then not Is_Void_Tree (TI_W.Dims (1).Idx_Type) then
                                                   declare
                                                      IK : constant Unsigned_32 := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, TI_W.Dims (1).Idx_Type)));
                                                   begin
                                                      if Ctx.Saved_Type_Ranges.Contains (IK) then
                                                         Ctx.Saved_Type_Ranges.Include (CID_K, Ctx.Saved_Type_Ranges.Element (IK));
                                                      end if;
                                                   end;
                                                end if;
                                             end;
                                          end if;
                                       end if;
                                    end;
                                 end loop;
                              end;
                           end if;
                        end loop;
                     end;
                  end if;
                  VP := D (AS_VARIANT_PART, CList);
                  if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART
                    and then not Is_Void_Tree (D (AS_VARIANT_S, VP))
                  then
                     declare
                        VL : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                        VN : TREE;
                     begin
                        while not IS_EMPTY (VL) loop
                           POP (VL, VN);
                           if KIND (VN) = DN_VARIANT then
                              Walk_CL (D (AS_COMP_LIST, VN));
                           end if;
                        end loop;
                     end;
                  end if;
               end Walk_CL;
            begin
               if not Is_Void_Tree (CL) and then KIND (CL) = DN_COMP_LIST then
                  Walk_CL (CL);
               end if;
            end;
         when others =>
            null;
      end case;
   end Emit_Type_Elab_Checks;

   function Emit_Actual_Constrained (Ctx : in out Context; Act : TREE) return String is
      N   : TREE := Act;
      Con : Boolean := True;

      function Root_Is_Always_Constrained (Expr : TREE) return Boolean is
         Cur : TREE := Expr;
      begin
         for Step in 1 .. 32 loop
            exit when Is_Void_Tree (Cur);
            case KIND (Cur) is
               when DN_PARENTHESIZED =>
                  Cur := D (AS_EXP, Cur);
               when DN_CONVERSION =>
                  declare
                     CT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Cur)));
                  begin
                     if not Is_Void_Tree (CT) and then KIND (CT) = DN_CONSTRAINED_RECORD then
                        return True;
                     end if;
                  end;
                  Cur := D (AS_EXP, Cur);
               when DN_FUNCTION_CALL | DN_NULLARY_CALL | DN_AGGREGATE | DN_QUALIFIED =>
                  return True;
               when DN_ALL =>
                  -- LRM 3.7.4(3): P.all itself is constrained (handled at top level),
                  -- but subcomponents of an allocated object (P.all.Comp, P.Comp, P_Arr(I))
                  -- are NOT constrained unless their own subtype is constrained!
                  return False;
               when DN_SELECTED =>
                  declare
                     Des : constant TREE := D (AS_DESIGNATOR, Cur);
                     Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des)));
                  begin
                     if not Is_Void_Tree (Def) and then KIND (Def) not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                        -- Package.Obj
                        Cur := Des;
                     else
                        -- Record component: implicit access deref does NOT constrain a subcomponent (LRM 3.7.4(3))
                        declare
                           Pfx_E : constant TREE := D (AS_NAME, Cur);
                           Pfx_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                        begin
                           if Is_Access_Type (Pfx_T) then
                              return False;
                           end if;
                           Cur := Pfx_E;
                        end;
                     end if;
                  end;
               when DN_INDEXED | DN_SLICE =>
                  declare
                     Pfx_E : constant TREE := D (AS_NAME, Cur);
                     Pfx_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                  begin
                     if Is_Access_Type (Pfx_T) then
                        return False;
                     end if;
                     Cur := Pfx_E;
                  end;
               when DN_USED_OBJECT_ID =>
                  declare
                     Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Cur)));
                  begin
                     if not Is_Void_Tree (Def) and then KIND (Def) in DN_CONSTANT_ID | DN_IN_ID then
                        return True;
                     end if;
                     return False;
                  end;
               when others =>
                  return False;
            end case;
         end loop;
         return False;
      end Root_Is_Always_Constrained;
   begin
      while not Is_Void_Tree (N) and then KIND (N) in DN_PARENTHESIZED | DN_CONVERSION loop
         -- P3-3.4: View conversion to a constrained record subtype IS constrained!
         if KIND (N) = DN_CONVERSION then
            declare
               CT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, N)));
            begin
               if not Is_Void_Tree (CT) and then KIND (CT) = DN_CONSTRAINED_RECORD then
                  declare
                     RC : constant String := Next_Reg (Ctx.E, "acon");
                  begin
                     Emit_Inst (Ctx.E, RC & " = const.i8 1");
                     return RC;
                  end;
               end if;
            end;
         end if;
         N := D (AS_EXP, N);
      end loop;
      if not Is_Void_Tree (N) then
         if KIND (N) = DN_USED_OBJECT_ID
           or else (KIND (N) = DN_SELECTED
                    and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, N)))
                    and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, N)))))
                             not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID)
         then
            declare
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (N)));
            begin
               if not Is_Void_Tree (Def) and then Ctx.Vars.Contains (Tree_Key (Def)) then
                  declare
                     VI : constant Var_Info := Ctx.Vars.Element (Tree_Key (Def));
                  begin
                     if VI.Owner_Subp = Ctx.Cur_Subp_Key and then Length (VI.Constrained_Reg) > 0 then
                        return To_String (VI.Constrained_Reg);
                     elsif Length (VI.Con_Global) > 0 then
                        declare
                           GA : constant String := Next_Reg (Ctx.E, "cga");
                           GV : constant String := Next_Reg (Ctx.E, "cgv");
                        begin
                           Emit_Inst (Ctx.E, GA & " = addr " & To_String (VI.Con_Global));
                           Emit_Inst (Ctx.E, GV & " = load.i8 " & GA & ", align 1");
                           return GV;
                        end;
                     elsif VI.Con_Captured_Slot > 0 and then VI.Owner_Subp /= 0 and then VI.Owner_Subp /= Ctx.Cur_Subp_Key then
                        declare
                           FP : constant String := Emit_Captured_Slot_Ptr (Ctx, VI.Owner_Subp, VI.Con_Captured_Slot);
                           CP : constant String := Next_Reg (Ctx.E, "ccp");
                           CV : constant String := Next_Reg (Ctx.E, "ccv");
                        begin
                           Emit_Inst (Ctx.E, CP & " = load.ptr " & FP & ", align 8");
                           Emit_Inst (Ctx.E, CV & " = load.i8 " & CP & ", align 1");
                           return CV;
                        end;
                     end if;
                  end;
               end if;
               if not Is_Void_Tree (Def) then
                  if KIND (Def) = DN_CONSTANT_ID or else KIND (Def) = DN_IN_ID then
                     Con := True;
                  else
                     declare
                        TS  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def)));
                        VTS : constant TREE :=
                          (if Ctx.Vars.Contains (Tree_Key (Def))
                           then Resolve_Type_Spec (Subst_Tree (Ctx, Ctx.Vars.Element (Tree_Key (Def)).Ada_Type))
                           else CONST_VOID);
                     begin
                        if (not Is_Void_Tree (VTS) and then KIND (VTS) = DN_CONSTRAINED_RECORD)
                          or else (not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_RECORD)
                        then
                           Con := True;
                        elsif Record_Has_Default_Discs (Ctx, TS) then
                           Con := False;
                        else
                           Con := True;
                        end if;
                     end;
                  end if;
               end if;
            end;
         elsif KIND (N) in DN_ALL | DN_FUNCTION_CALL | DN_NULLARY_CALL | DN_AGGREGATE | DN_QUALIFIED then
            Con := True;
         elsif KIND (N) in DN_SELECTED | DN_INDEXED | DN_SLICE then
            declare
               TS : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, N)));
            begin
               if Root_Is_Always_Constrained (N) then
                  Con := True;
               elsif not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_RECORD then
                  Con := True;
               elsif Record_Has_Default_Discs (Ctx, TS) then
                  Con := False;
               else
                  Con := True;
               end if;
            end;
         end if;
      end if;
      declare
         R : constant String := Next_Reg (Ctx.E, "acon");
      begin
         Emit_Inst (Ctx.E, R & " = const.i8 " & (if Con then "1" else "0"));
         return R;
      end;
   end Emit_Actual_Constrained;

end Cg_Expr;
