-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with Ada.Strings.Fixed;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;

package body Cg_Expr is

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
     (Ctx      : in out Context;
      Val_Reg  : String;
      From_Ty  : String;
      To_Ty    : String) return String
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
         begin
            Emit_Inst (Ctx.E, R & " = sext." & To_Ty & " " & Val_Reg);
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
     (Ctx      : in out Context;
      Val_Reg  : String;
      From_Ty  : String;
      To_Ty    : String) return String
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
            R : constant String := Next_Reg (Ctx.E, "i2f");
         begin
            Emit_Inst (Ctx.E, R & " = sitofp." & To_Ty & " " & Val_Reg);
            return R;
         end;
      elsif (From_Ty = "f64" or else From_Ty = "f32") and then To_Ty /= "ptr" then
         declare
            R : constant String := Next_Reg (Ctx.E, "f2i");
         begin
            Emit_Inst (Ctx.E, R & " = fptosi." & To_Ty & " " & Val_Reg & " [sat]");
            return R;
         end;
      else
         return Emit_Int_Convert (Ctx, Val_Reg, From_Ty, To_Ty);
      end if;
   end Emit_Scalar_Coerce;

   procedure Emit_Check_Fail_If (Ctx : in out Context; Cond_Reg : String) is
      Fail_L : constant String := Next_Label (Ctx.E, "chk_fail");
      Ok_L   : constant String := Next_Label (Ctx.E, "chk_ok");
   begin
      Emit_Term (Ctx.E, "br " & Cond_Reg & ", " & Fail_L & ", " & Ok_L);
      Emit_Label (Ctx.E, Fail_L);
      Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
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
            Tag  : constant String := Next_Reg (Ctx.E, "sv");
            GSym : constant String := "@ada_" & Tag (Tag'First + 1 .. Tag'Last);
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
      if not Ctx.Enable_Checks or else Is_Void_Tree (R) then
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
                  L_R : constant String := Emit_Scalar_Coerce (Ctx, LR0, To_String (SR.Pir_Type), Val_Pir_Ty);
                  H_R : constant String := Emit_Scalar_Coerce (Ctx, HR0, To_String (SR.Pir_Type), Val_Pir_Ty);
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
                     Emit_Inst (Ctx.E, C1 & " = cmp_lt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                     Emit_Inst (Ctx.E, C2 & " = cmp_gt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
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
                        F1 : constant IEEE_Float_64 := Eval_Float64 (V1) / Sm;
                        F2 : constant IEEE_Float_64 := Eval_Float64 (V2) / Sm;
                        I1, I2 : Integer_64;
                     begin
                        if F1 <= IEEE_Float_64 (Integer_64'First) then I1 := Integer_64'First;
                        elsif F1 >= IEEE_Float_64 (Integer_64'Last) then I1 := Integer_64'Last;
                        else I1 := Integer_64 (IEEE_Float_64'Rounding (F1));
                        end if;
                        if F2 <= IEEE_Float_64 (Integer_64'First) then I2 := Integer_64'First;
                        elsif F2 >= IEEE_Float_64 (Integer_64'Last) then I2 := Integer_64'Last;
                        else I2 := Integer_64 (IEEE_Float_64'Rounding (F2));
                        end if;
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
                           declare
                              F64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), "f64");
                              Dv  : constant String := Next_Reg (Ctx.E, "fxld");
                              Iv  : constant String := Next_Reg (Ctx.E, "fxli");
                           begin
                              Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & F64 & ", " & Format_Float64 (Sm));
                              Emit_Inst (Ctx.E, Iv & " = fptosi." & Val_Pir_Ty & " " & Dv & " [sat]");
                              L_R := To_Unbounded_String (Iv);
                           end;
                        else
                           L_R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), Val_Pir_Ty));
                        end if;
                        if To_String (EV2.Pir_Type) = "f32" or else To_String (EV2.Pir_Type) = "f64" then
                           declare
                              F64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV2.Reg), To_String (EV2.Pir_Type), "f64");
                              Dv  : constant String := Next_Reg (Ctx.E, "fxhd");
                              Iv  : constant String := Next_Reg (Ctx.E, "fxhi");
                           begin
                              Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & F64 & ", " & Format_Float64 (Sm));
                              Emit_Inst (Ctx.E, Iv & " = fptosi." & Val_Pir_Ty & " " & Dv & " [sat]");
                              H_R := To_Unbounded_String (Iv);
                           end;
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
                        F1  : constant IEEE_Float_64 := Eval_Float64 (V1);
                        F2  : constant IEEE_Float_64 := Eval_Float64 (V2);
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
                     L_R : constant String := Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), Val_Pir_Ty);
                     H_R : constant String := Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), Val_Pir_Ty);
                     C1  : constant String := Next_Reg (Ctx.E, "rlo");
                     C2  : constant String := Next_Reg (Ctx.E, "rhi");
                     Bad : constant String := Next_Reg (Ctx.E, "rbad");
                  begin
                     Emit_Inst (Ctx.E, C1 & " = cmp_lt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                     Emit_Inst (Ctx.E, C2 & " = cmp_gt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               elsif not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE_ATTRIBUTE then
                  declare
                     RT_U, Lo_U, Hi_U : Unbounded_String;
                  begin
                     Emit_Discrete_Range (Ctx, Rng, RT_U, Lo_U, Hi_U);
                     declare
                        L_R : constant String := Emit_Int_Convert (Ctx, To_String (Lo_U), To_String (RT_U), Val_Pir_Ty);
                        H_R : constant String := Emit_Int_Convert (Ctx, To_String (Hi_U), To_String (RT_U), Val_Pir_Ty);
                        C1  : constant String := Next_Reg (Ctx.E, "rlo");
                        C2  : constant String := Next_Reg (Ctx.E, "rhi");
                        Bad : constant String := Next_Reg (Ctx.E, "rbad");
                     begin
                        Emit_Inst (Ctx.E, C1 & " = cmp_lt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                        Emit_Inst (Ctx.E, C2 & " = cmp_gt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
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
         Bits : constant Natural := Int_Bits_Of_Pir (Val_Pir_Ty);
         Full_Lo : Integer_64;
         Full_Hi : Integer_64;
      begin
         case Bits is
            when 8  => Full_Lo := -128; Full_Hi := 127;
            when 16 => Full_Lo := -32768; Full_Hi := 32767;
            when 32 => Full_Lo := -2147483648; Full_Hi := 2147483647;
            when others => Full_Lo := Integer_64'First; Full_Hi := Integer_64'Last;
         end case;
         if Lo <= Full_Lo and then Hi >= Full_Hi then
            return;
         end if;
         -- For unsigned-like enums (e.g. Boolean 0..1, Character 0..127) on i8:
         declare
            Need_Lo : constant Boolean := Lo > Full_Lo;
            Need_Hi : constant Boolean := Hi < Full_Hi;
            C1, C2, Bad : Unbounded_String;
         begin
            if Need_Lo and then Need_Hi then
               C1  := To_Unbounded_String (Next_Reg (Ctx.E, "rlo"));
               C2  := To_Unbounded_String (Next_Reg (Ctx.E, "rhi"));
               Bad := To_Unbounded_String (Next_Reg (Ctx.E, "rbad"));
               Emit_Inst (Ctx.E, To_String (C1) & " = cmp_lt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Lo)));
               Emit_Inst (Ctx.E, To_String (C2) & " = cmp_gt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Hi)));
               Emit_Inst (Ctx.E, To_String (Bad) & " = or.i8 " & To_String (C1) & ", " & To_String (C2));
               Emit_Check_Fail_If (Ctx, To_String (Bad));
            elsif Need_Lo then
               C1 := To_Unbounded_String (Next_Reg (Ctx.E, "rlo"));
               Emit_Inst (Ctx.E, To_String (C1) & " = cmp_lt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Lo)));
               Emit_Check_Fail_If (Ctx, To_String (C1));
            elsif Need_Hi then
               C2 := To_Unbounded_String (Next_Reg (Ctx.E, "rhi"));
               Emit_Inst (Ctx.E, To_String (C2) & " = cmp_gt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Hi)));
               Emit_Check_Fail_If (Ctx, To_String (C2));
            end if;
         end;
      end;
   end Emit_Range_Check;

   function Expr_Has_Unbound_Discr (Ctx : Context; Node : TREE; Depth : Natural := 0) return Boolean;
   function Is_Access_To_Uncon_Array (T : TREE) return Boolean;
   procedure Emit_Unpack_Arr_Desc
     (Ctx      : in out Context;
      Desc_Ptr : String;
      Des_T    : TREE;
      Out_Val  : out Expr_Val);

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
            declare
               RF : constant String := Next_Reg (Ctx.E, "af");
               RL : constant String := Next_Reg (Ctx.E, "al");
            begin
               Emit_Inst (Ctx.E, RF & " = const.i32 " & Img (Long_Long_Integer (TI.Dims (Dim).First)));
               Emit_Inst (Ctx.E, RL & " = const.i32 " & Img (Long_Long_Integer (TI.Dims (Dim).Last)));
               First_Reg := To_Unbounded_String (RF);
               Last_Reg  := To_Unbounded_String (RL);
            end;
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
         Emit_Inst (Ctx.E, RF & " = const.i32 1");
         Emit_Inst (Ctx.E, RL & " = const.i32 0");
         First_Reg := To_Unbounded_String (RF);
         Last_Reg  := To_Unbounded_String (RL);
      end;
   end Get_Array_Dim_Bounds;

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
               Low_Reg  := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), To_String (Pir_Ty)));
               High_Reg := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), To_String (Pir_Ty)));
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
                                 NE  : constant String := Next_Reg (Ctx.E, "dsne");
                                 L_C : constant String := Next_Label (Ctx.E, "dsc");
                                 L_O : constant String := Next_Label (Ctx.E, "dso");
                              begin
                                 Emit_Inst (Ctx.E, NE & " = cmp_le_s." & To_String (Pir_Ty) & " " & To_String (Low_Reg) & ", " & To_String (High_Reg));
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
                  TI  : constant Type_Info := Get_Type_Info (Ctx, TS);
                  St  : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Pir_Ty := TI.Pir_Type;
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
                     Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (TS)), Pir_Ty, Low_Reg, High_Reg);
                  end if;
               end;
            else
               declare
                  Def : constant TREE := Get_Name_Defn (SI);
                  TS  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def));
                  TI  : constant Type_Info := Get_Type_Info (Ctx, TS);
                  St  : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Pir_Ty := TI.Pir_Type;
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
                     Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (TS)), Pir_Ty, Low_Reg, High_Reg);
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
            -- Prefix can be an array object or an array type mark
            if KIND (Pfx) = DN_USED_NAME_ID and then
              KIND (D (SM_DEFN, Pfx)) in CLASS_TYPE_NAME
            then
               declare
                  TS : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (SM_DEFN, Pfx)));
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
                     Low_Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F32), "i32", To_String (Pir_Ty)));
                     High_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L32), "i32", To_String (Pir_Ty)));
                  end;
               end;
            end if;
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
                  LR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                  HR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
               begin
                  First_R := To_Unbounded_String (Emit_Int_Convert (Ctx, LR0, To_String (SR.Pir_Type), "i32"));
                  Last_R  := To_Unbounded_String (Emit_Int_Convert (Ctx, HR0, To_String (SR.Pir_Type), "i32"));
                  return;
               end;
            end if;
         end;
      end if;
      if not Is_Void_Tree (IT) then
         Get_Discrete_Bounds (IT, St, Lo, Hi);
      end if;
      if St then
         declare
            RF : constant String := Next_Reg (Ctx.E, "ibf");
            RL : constant String := Next_Reg (Ctx.E, "ibl");
         begin
            Emit_Inst (Ctx.E, RF & " = const.i32 " & Img (Long_Long_Integer (Lo)));
            Emit_Inst (Ctx.E, RL & " = const.i32 " & Img (Long_Long_Integer (Hi)));
            First_R := To_Unbounded_String (RF);
            Last_R  := To_Unbounded_String (RL);
         end;
      elsif not Is_Void_Tree (IT) and then not Is_Void_Tree (D (SM_RANGE, IT)) then
         declare
            RT, RL, RH : Unbounded_String;
         begin
            Emit_Discrete_Range (Ctx, D (SM_RANGE, IT), RT, RL, RH);
            First_R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
            Last_R  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
         end;
      else
         declare
            RF : constant String := Next_Reg (Ctx.E, "ibf");
            RL : constant String := Next_Reg (Ctx.E, "ibl");
         begin
            Emit_Inst (Ctx.E, RF & " = const.i32 1");
            Emit_Inst (Ctx.E, RL & " = const.i32 0");
            First_R := To_Unbounded_String (RF);
            Last_R  := To_Unbounded_String (RL);
         end;
      end if;
   end Emit_Index_Bounds;

   function Emit_Dim_Len_I64
     (Ctx      : in out Context;
      First_32 : String;
      Last_32  : String) return String
   is
      F64 : constant String := Next_Reg (Ctx.E, "dlf");
      L64 : constant String := Next_Reg (Ctx.E, "dll");
      Ge  : constant String := Next_Reg (Ctx.E, "dlg");
      D64 : constant String := Next_Reg (Ctx.E, "dld");
      N64 : constant String := Next_Reg (Ctx.E, "dln");
      Res : constant String := Next_Reg (Ctx.E, "dlr");
   begin
      Emit_Inst (Ctx.E, F64 & " = sext.i64 " & First_32);
      Emit_Inst (Ctx.E, L64 & " = sext.i64 " & Last_32);
      Emit_Inst (Ctx.E, Ge  & " = cmp_ge_s.i64 " & L64 & ", " & F64);
      Emit_Inst (Ctx.E, D64 & " = sub.i64 " & L64 & ", " & F64);
      Emit_Inst (Ctx.E, N64 & " = add.i64 " & D64 & ", 1");
      Emit_Inst (Ctx.E, Res & " = select.i64 " & Ge & ", " & N64 & ", 0");
      return Res;
   end Emit_Dim_Len_I64;

   procedure Emit_Array_Length_Check
     (Ctx   : in out Context;
      LHS   : Expr_Val;
      RHS   : Expr_Val;
      NDims : Natural)
   is
      ND : constant Natural := Natural'Max (1, Natural'Min (NDims, 4));
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

   procedure Emit_Unpack_Arr_Desc
     (Ctx      : in out Context;
      Desc_Ptr : String;
      Des_T    : TREE;
      Out_Val  : out Expr_Val)
   is
      DTI : constant Type_Info := Get_Type_Info (Ctx, Des_T);
      ETI : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
      ND  : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, 4));
      P0  : constant String := Next_Reg (Ctx.E, "adp");
      D0  : constant String := Next_Reg (Ctx.E, "add");
   begin
      Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & Desc_Ptr & ", 0");
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
            Emit_Inst (Ctx.E, FP & " = field_ptr @__ada_arr_desc, " & Desc_Ptr & ", " & Img (2 * D_Idx - 1));
            Emit_Inst (Ctx.E, FV & " = load.i32 " & FP & ", align 4");
            Emit_Inst (Ctx.E, LP & " = field_ptr @__ada_arr_desc, " & Desc_Ptr & ", " & Img (2 * D_Idx));
            Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
            Out_Val.First_Regs (D_Idx) := To_Unbounded_String (FV);
            Out_Val.Last_Regs (D_Idx)  := To_Unbounded_String (LV);
         end;
      end loop;
   end Emit_Unpack_Arr_Desc;

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
            while not IS_EMPTY (IL) and then Dim < 4 loop
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
                  Emit_Inst (Ctx.E, FV & " = load.i32 " & FP & ", align 4");
                  Emit_Inst (Ctx.E, LP & " = field_ptr @__ada_arr_desc, " & Rec_Ptr & ", " & Img (2 * Dim));
                  Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
                  Emit_Index_Bounds (Ctx, Idx_T, TF, TL);
                  Emit_Inst (Ctx.E, CF & " = cmp_ne.i32 " & FV & ", " & To_String (TF));
                  Emit_Inst (Ctx.E, CL & " = cmp_ne.i32 " & LV & ", " & To_String (TL));
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
         BT  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (RT));
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
                           Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & Rec_Ptr & ", " & Img (FIdx));
                           Emit_Inst (Ctx.E, Act & " = load." & To_String (CTI.Pir_Type) & " " & FP & ", align " & Img (CTI.Align));
                           Emit_Inst (Ctx.E, Neq & " = cmp_ne." & To_String (CTI.Pir_Type) & " " & Act & ", " & To_String (ER));
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
               A1 : constant String := Emit_Int_Convert (Ctx, To_String (L1), To_String (T1), "i64");
               B1 : constant String := Emit_Int_Convert (Ctx, To_String (H1), To_String (T1), "i64");
               A2 : constant String := Emit_Int_Convert (Ctx, To_String (L2), To_String (T2), "i64");
               B2 : constant String := Emit_Int_Convert (Ctx, To_String (H2), To_String (T2), "i64");
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
                  Emit_Inst (Ctx.E, N1 & " = cmp_gt_s.i32 " & To_String (F1) & ", " & To_String (L1));
                  Emit_Inst (Ctx.E, N2 & " = cmp_gt_s.i32 " & To_String (F2) & ", " & To_String (L2));
                  Emit_Inst (Ctx.E, BN & " = and.i8 " & N1 & ", " & N2);
                  Emit_Inst (Ctx.E, E1 & " = cmp_eq.i32 " & To_String (F1) & ", " & To_String (F2));
                  Emit_Inst (Ctx.E, E2 & " = cmp_eq.i32 " & To_String (L1) & ", " & To_String (L2));
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
                        A1 : constant String := Emit_Int_Convert (Ctx, To_String (V1L.Reg), To_String (V1L.Pir_Type), "i64");
                        B1 : constant String := Emit_Int_Convert (Ctx, To_String (V1H.Reg), To_String (V1H.Pir_Type), "i64");
                        A2 : constant String := Emit_Int_Convert (Ctx, To_String (V2L.Reg), To_String (V2L.Pir_Type), "i64");
                        B2 : constant String := Emit_Int_Convert (Ctx, To_String (V2H.Reg), To_String (V2H.Pir_Type), "i64");
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
                              P0 : constant String := Next_Reg (Ctx.E, "dp0");
                              D0 : constant String := Next_Reg (Ctx.E, "dd0");
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
                              Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & To_String (DP) & ", 0");
                              Emit_Inst (Ctx.E, D0 & " = load.ptr " & P0 & ", align 8");
                              Res.Reg := To_Unbounded_String (D0);
                              for D_Idx in 1 .. Natural'Min (V.NDims, 4) loop
                                 declare
                                    FP : constant String := Next_Reg (Ctx.E, "dfp");
                                    FV : constant String := Next_Reg (Ctx.E, "dfv");
                                    LP : constant String := Next_Reg (Ctx.E, "dlp");
                                    LV : constant String := Next_Reg (Ctx.E, "dlv");
                                 begin
                                    Emit_Inst (Ctx.E, FP & " = field_ptr @__ada_arr_desc, " & To_String (DP) & ", " & Img (2 * D_Idx - 1));
                                    Emit_Inst (Ctx.E, FV & " = load.i32 " & FP & ", align 4");
                                    Emit_Inst (Ctx.E, LP & " = field_ptr @__ada_arr_desc, " & To_String (DP) & ", " & Img (2 * D_Idx));
                                    Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
                                    Res.First_Regs (D_Idx) := To_Unbounded_String (FV);
                                    Res.Last_Regs (D_Idx)  := To_Unbounded_String (LV);
                                 end;
                              end loop;
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
                  if KIND (Def) in DN_TYPE_ID | DN_TASK_BODY_ID then
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
                                 P0   : constant String := Next_Reg (Ctx.E, "erd0");
                                 DP   : constant String := Next_Reg (Ctx.E, "erdp");
                                 ND   : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, 4));
                              begin
                                 Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global @__ada_arr_desc, align 8");
                                 Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                                 Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & AR & ", 0");
                                 Emit_Inst (Ctx.E, DP & " = load.ptr " & P0 & ", align 8");
                                 Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (DP);
                                 Res.Pir_Type := To_Unbounded_String ("ptr");
                                 Res.Ada_Type := ATy; Res.Align := TI.Align; Res.NDims := ND;
                                 for D_I in 1 .. ND loop
                                    declare
                                       FF : constant String := Next_Reg (Ctx.E, "erdf");
                                       LF : constant String := Next_Reg (Ctx.E, "erdl");
                                       FV : constant String := Next_Reg (Ctx.E, "erfv");
                                       LV : constant String := Next_Reg (Ctx.E, "erlv");
                                    begin
                                       Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & AR & ", " & Img (2 * D_I - 1));
                                       Emit_Inst (Ctx.E, FV & " = load.i32 " & FF & ", align 4");
                                       Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & AR & ", " & Img (2 * D_I));
                                       Emit_Inst (Ctx.E, LV & " = load.i32 " & LF & ", align 4");
                                       Res.First_Regs (D_I) := To_Unbounded_String (FV);
                                       Res.Last_Regs (D_I)  := To_Unbounded_String (LV);
                                    end;
                                 end loop;
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
                           P0   : constant String := Next_Reg (Ctx.E, "egd0");
                           DP   : constant String := Next_Reg (Ctx.E, "egdp");
                           ND   : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, 4));
                        begin
                           Add_Global_Decl (Ctx.E, GSym, "extern " & GSym & " = global @__ada_arr_desc, align 8");
                           Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                           Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & AR & ", 0");
                           Emit_Inst (Ctx.E, DP & " = load.ptr " & P0 & ", align 8");
                           Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (DP);
                           Res.Pir_Type := To_Unbounded_String ("ptr");
                           Res.Ada_Type := ATy; Res.Align := TI.Align; Res.NDims := ND;
                           for D_I in 1 .. ND loop
                              declare
                                 FF : constant String := Next_Reg (Ctx.E, "egdf");
                                 LF : constant String := Next_Reg (Ctx.E, "egdl");
                                 FV : constant String := Next_Reg (Ctx.E, "egfv");
                                 LV : constant String := Next_Reg (Ctx.E, "eglv");
                              begin
                                 Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & AR & ", " & Img (2 * D_I - 1));
                                 Emit_Inst (Ctx.E, FV & " = load.i32 " & FF & ", align 4");
                                 Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & AR & ", " & Img (2 * D_I));
                                 Emit_Inst (Ctx.E, LV & " = load.i32 " & LF & ", align 4");
                                 Res.First_Regs (D_I) := To_Unbounded_String (FV);
                                 Res.Last_Regs (D_I)  := To_Unbounded_String (LV);
                              end;
                           end loop;
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
                           Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8");
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
                           begin
                              if not Is_Void_Tree (Base_R) and then KIND (Base_R) = DN_RECORD then
                                 Find_In_List (D (SM_DISCRIMINANT_S, Base_R));
                                 if not Ctx.Fields.Contains (Use_CK) and then not Is_Void_Tree (D (SM_COMP_LIST, Base_R)) then
                                    Find_In_List (D (AS_DECL_S, D (SM_COMP_LIST, Base_R)));
                                 end if;
                              end if;
                           end;
                        end if;
                     if Ctx.Fields.Contains (Use_CK) then
                        declare
                           FE : constant Field_Entry := Ctx.Fields.Element (Use_CK);
                        begin
                           if not FE.In_Union and then not FE.Has_Explicit_Offset then
                              Emit_Inst (Ctx.E,
                                FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " &
                                To_String (Rec_Ptr) & ", " & Img (FE.Field_Idx));
                           else
                              -- LRM 4.1.3(16) (c41304b): check active discriminant matches variant containing Def!
                              if FE.In_Union and then Ctx.Enable_Checks then
                                 declare
                                    Base_R : constant TREE := Subst_Base_Type (Ctx, Rec_T);
                                    Target_Def : constant TREE := Canonical_Def (Def);
                                    function Comp_List_Contains (CL : TREE) return Boolean is
                                    begin
                                       if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return False; end if;
                                       if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                                          declare
                                             DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                                             DI : TREE;
                                          begin
                                             while not IS_EMPTY (DL) loop
                                                POP (DL, DI);
                                                if KIND (DI) = DN_VARIABLE_DECL then
                                                   declare
                                                      NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DI));
                                                      ID : TREE;
                                                   begin
                                                      while not IS_EMPTY (NS) loop
                                                         POP (NS, ID);
                                                         if Canonical_Def (ID) = Target_Def or else Tree_Key (ID) = Use_CK then
                                                            return True;
                                                         end if;
                                                      end loop;
                                                   end;
                                                end if;
                                             end loop;
                                          end;
                                       end if;
                                       declare
                                          VP2 : constant TREE := D (AS_VARIANT_PART, CL);
                                       begin
                                          if not Is_Void_Tree (VP2) and then KIND (VP2) = DN_VARIANT_PART
                                            and then not Is_Void_Tree (D (AS_VARIANT_S, VP2))
                                          then
                                             declare
                                                VL2 : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP2));
                                                VN2 : TREE;
                                             begin
                                                while not IS_EMPTY (VL2) loop
                                                   POP (VL2, VN2);
                                                   if KIND (VN2) = DN_VARIANT and then Comp_List_Contains (D (AS_COMP_LIST, VN2)) then
                                                      return True;
                                                   end if;
                                                end loop;
                                             end;
                                          end if;
                                       end;
                                       return False;
                                    end Comp_List_Contains;

                                    procedure Check_Variant_Chain (CL : TREE) is
                                       VP2 : TREE;
                                    begin
                                       if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return; end if;
                                       VP2 := D (AS_VARIANT_PART, CL);
                                       if Is_Void_Tree (VP2) or else KIND (VP2) /= DN_VARIANT_PART
                                         or else Is_Void_Tree (D (AS_VARIANT_S, VP2))
                                       then
                                          return;
                                       end if;
                                       declare
                                          D_Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, VP2))));
                                          DK    : constant Unsigned_32 := Tree_Key (D_Def);
                                          VL2   : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP2));
                                          VN2   : TREE;
                                          Matched_V : TREE := CONST_VOID;
                                          Other_Cond : Unbounded_String;
                                          Is_Others_V : Boolean := False;
                                       begin
                                          if not Ctx.Fields.Contains (DK) then return; end if;
                                          declare
                                             DFE  : constant Field_Entry := Ctx.Fields.Element (DK);
                                             DT_A : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, D_Def));
                                             DTI_D : constant Type_Info := Get_Type_Info (Ctx, DT_A);
                                             DP   : constant String := Next_Reg (Ctx.E, "svdp");
                                             DV   : constant String := Next_Reg (Ctx.E, "svdv");
                                             DV64 : Unbounded_String;
                                             function Eval_Choice_List (CS_N : TREE) return String is
                                                CS : SEQ_TYPE := LIST (CS_N);
                                                Ch : TREE;
                                                Hit : Unbounded_String;
                                             begin
                                                while not IS_EMPTY (CS) loop
                                                   POP (CS, Ch);
                                                   if KIND (Ch) = DN_CHOICE_EXP then
                                                      declare
                                                         CV  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch), DT_A);
                                                         C64 : constant String := Emit_Int_Convert (Ctx, To_String (CV.Reg), To_String (CV.Pir_Type), "i64");
                                                         Eq  : constant String := Next_Reg (Ctx.E, "svce");
                                                      begin
                                                         Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & To_String (DV64) & ", " & C64);
                                                         if Length (Hit) = 0 then Hit := To_Unbounded_String (Eq);
                                                         else
                                                            declare
                                                               O : constant String := Next_Reg (Ctx.E, "svco");
                                                            begin
                                                               Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & Eq);
                                                               Hit := To_Unbounded_String (O);
                                                            end;
                                                         end if;
                                                      end;
                                                   elsif KIND (Ch) = DN_CHOICE_RANGE then
                                                      declare
                                                         RT2, LoR, HiR : Unbounded_String;
                                                         Lo64, Hi64 : Unbounded_String;
                                                         Ge : constant String := Next_Reg (Ctx.E, "svge");
                                                         Le : constant String := Next_Reg (Ctx.E, "svle");
                                                         InR : constant String := Next_Reg (Ctx.E, "svin");
                                                      begin
                                                         Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT2, LoR, HiR);
                                                         Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LoR), To_String (RT2), "i64"));
                                                         Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (HiR), To_String (RT2), "i64"));
                                                         Emit_Inst (Ctx.E, Ge & " = cmp_ge_s.i64 " & To_String (DV64) & ", " & To_String (Lo64));
                                                         Emit_Inst (Ctx.E, Le & " = cmp_le_s.i64 " & To_String (DV64) & ", " & To_String (Hi64));
                                                         Emit_Inst (Ctx.E, InR & " = and.i8 " & Ge & ", " & Le);
                                                         if Length (Hit) = 0 then Hit := To_Unbounded_String (InR);
                                                         else
                                                            declare
                                                               O : constant String := Next_Reg (Ctx.E, "svco");
                                                            begin
                                                               Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & InR);
                                                               Hit := To_Unbounded_String (O);
                                                            end;
                                                         end if;
                                                      end;
                                                   end if;
                                                end loop;
                                                return To_String (Hit);
                                             end Eval_Choice_List;
                                          begin
                                             Emit_Inst (Ctx.E, DP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (Rec_Ptr) & ", " & Img (DFE.Field_Idx));
                                             Emit_Inst (Ctx.E, DV & " = load." & To_String (DTI_D.Pir_Type) & " " & DP & ", align " & Img (Natural'Max (1, DTI_D.Align)));
                                             DV64 := To_Unbounded_String (Emit_Int_Convert (Ctx, DV, To_String (DTI_D.Pir_Type), "i64"));
                                             while not IS_EMPTY (VL2) loop
                                                POP (VL2, VN2);
                                                if KIND (VN2) = DN_VARIANT then
                                                   if Comp_List_Contains (D (AS_COMP_LIST, VN2)) then
                                                      Matched_V := VN2;
                                                      declare
                                                         CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN2));
                                                         Ch : TREE;
                                                      begin
                                                         while not IS_EMPTY (CS) loop
                                                            POP (CS, Ch);
                                                            if KIND (Ch) = DN_CHOICE_OTHERS then Is_Others_V := True; end if;
                                                         end loop;
                                                      end;
                                                   else
                                                      declare
                                                         OC : constant String := Eval_Choice_List (D (AS_CHOICE_S, VN2));
                                                      begin
                                                         if OC'Length > 0 then
                                                            if Length (Other_Cond) = 0 then
                                                               Other_Cond := To_Unbounded_String (OC);
                                                            else
                                                               declare
                                                                  O : constant String := Next_Reg (Ctx.E, "svoo");
                                                               begin
                                                                  Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Other_Cond) & ", " & OC);
                                                                  Other_Cond := To_Unbounded_String (O);
                                                               end;
                                                            end if;
                                                         end if;
                                                      end;
                                                   end if;
                                                end if;
                                             end loop;
                                             if not Is_Void_Tree (Matched_V) then
                                                if Is_Others_V then
                                                   if Length (Other_Cond) > 0 then
                                                      Emit_Check_Fail_If (Ctx, To_String (Other_Cond));
                                                   end if;
                                                else
                                                   declare
                                                      MC : constant String := Eval_Choice_List (D (AS_CHOICE_S, Matched_V));
                                                   begin
                                                      if MC'Length > 0 then
                                                         declare
                                                            Bad : constant String := Next_Reg (Ctx.E, "svbd");
                                                         begin
                                                            Emit_Inst (Ctx.E, Bad & " = xor.i8 " & MC & ", 1");
                                                            Emit_Check_Fail_If (Ctx, Bad);
                                                         end;
                                                      end if;
                                                   end;
                                                end if;
                                                Check_Variant_Chain (D (AS_COMP_LIST, Matched_V));
                                             end if;
                                          end;
                                       end;
                                    end Check_Variant_Chain;
                                 begin
                                    if not Is_Void_Tree (Base_R) and then KIND (Base_R) = DN_RECORD then
                                       Check_Variant_Chain (D (SM_COMP_LIST, Base_R));
                                    end if;
                                 end;
                              end if;
                              declare
                                 Off_R : constant String := Next_Reg (Ctx.E, "ufo");
                              begin
                                 Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                                 Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & To_String (Rec_Ptr) & ", " & Off_R & " [inbounds]");
                              end;
                           end if;
                        end;
                     else
                        Emit_Inst (Ctx.E,
                          FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " &
                          To_String (Rec_Ptr) & ", 0");
                     end if;
                     end;
                     if CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                        -- Embedded @__ada_arr_desc inside record: unpack into EK_Slice!
                        Emit_Unpack_Arr_Desc (Ctx, FP, CT, Res);
                        return Res;
                     end if;
                     Res.Kind     := EK_Addr;
                     Res.Reg      := To_Unbounded_String (FP);
                     Res.Pir_Type := CTI.Pir_Type;
                     Res.Ada_Type := CT;
                     Res.Align    := CTI.Align;
                     return Res;
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
                           Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Arr_Ptr) & ", align 8");
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
                        I32   : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i32");
                        F_Reg : Unbounded_String;
                        L_Reg : Unbounded_String;
                        Off32 : constant String := Next_Reg (Ctx.E, "ioff");
                        Off64 : constant String := Next_Reg (Ctx.E, "ioff64");
                     begin
                        Get_Array_Dim_Bounds (Ctx, Arr_Val, Dim, F_Reg, L_Reg);
                        declare
                           Skip_Chk : Boolean := not Ctx.Enable_Checks;
                        begin
                           if not Skip_Chk and then Dim <= ATI.NDims and then ATI.Dims (Dim).Is_Static then
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
                                 Emit_Inst (Ctx.E, C1 & " = cmp_lt_s.i32 " & I32 & ", " & To_String (F_Reg));
                                 Emit_Inst (Ctx.E, C2 & " = cmp_gt_s.i32 " & I32 & ", " & To_String (L_Reg));
                                 Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                 Emit_Check_Fail_If (Ctx, Bad);
                              end;
                           end if;
                        end;
                        Emit_Inst (Ctx.E, Off32 & " = sub.i32 " & I32 & ", " & To_String (F_Reg));
                        Emit_Inst (Ctx.E, Off64 & " = sext.i64 " & Off32);
                        if Dim < ATI.NDims then
                           -- Advance base pointer by row stride (enables LICM hoisting out of inner dim loops!)
                           declare
                              Stride_Elems : Unbounded_String;
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
                                    NP    : constant String := Next_Reg (Ctx.E, "rptr");
                                 begin
                                    Emit_Inst (Ctx.E, R_Off & " = mul.i64 " & Off64 & ", " & Img (Long_Long_Integer (Prod)));
                                    Cur_Ptr := To_Unbounded_String (Emit_Elem_Ptr (Ctx, ETI, To_String (Cur_Ptr), R_Off));
                                    pragma Unreferenced (NP);
                                 end;
                              else
                                 declare
                                    S_Acc : Unbounded_String := To_Unbounded_String (Off64);
                                 begin
                                    for Sub_D in Dim + 1 .. ATI.NDims loop
                                       declare
                                          SF, SL : Unbounded_String;
                                          Sp : constant String := Next_Reg (Ctx.E, "spn");
                                          Ln : constant String := Next_Reg (Ctx.E, "dlen");
                                          L6 : constant String := Next_Reg (Ctx.E, "dlen64");
                                          Ml : constant String := Next_Reg (Ctx.E, "fmul");
                                       begin
                                          Get_Array_Dim_Bounds (Ctx, Arr_Val, Sub_D, SF, SL);
                                          Emit_Inst (Ctx.E, Sp & " = sub.i32 " & To_String (SL) & ", " & To_String (SF));
                                          Emit_Inst (Ctx.E, Ln & " = add.i32 " & Sp & ", 1");
                                          Emit_Inst (Ctx.E, L6 & " = sext.i64 " & Ln);
                                          Emit_Inst (Ctx.E, Ml & " = mul.i64 " & To_String (S_Acc) & ", " & L6);
                                          S_Acc := To_Unbounded_String (Ml);
                                       end;
                                    end loop;
                                    declare
                                       NP : constant String := Next_Reg (Ctx.E, "rptr");
                                    begin
                                       Cur_Ptr := To_Unbounded_String (Emit_Elem_Ptr (Ctx, ETI, To_String (Cur_Ptr), To_String (S_Acc)));
                                       pragma Unreferenced (NP);
                                    end;
                                 end;
                              end if;
                              pragma Unreferenced (Stride_Elems);
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
               Lo32, Hi32 : Unbounded_String;
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
               Lo32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (Rng_T), "i32"));
               Hi32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (Rng_T), "i32"));
               Get_Array_Dim_Bounds (Ctx, Pfx_Val, 1, AF_R, AL_R);

               if Ctx.Enable_Checks then
                  -- Non-empty slice check: if Lo <= Hi then Lo >= AF and Hi <= AL
                  declare
                     Non_Empty : constant String := Next_Reg (Ctx.E, "sne");
                     C1        : constant String := Next_Reg (Ctx.E, "slo");
                     C2        : constant String := Next_Reg (Ctx.E, "shi");
                     Oob       : constant String := Next_Reg (Ctx.E, "soob");
                     Bad       : constant String := Next_Reg (Ctx.E, "sbad");
                  begin
                     Emit_Inst (Ctx.E, Non_Empty & " = cmp_le_s.i32 " & To_String (Lo32) & ", " & To_String (Hi32));
                     Emit_Inst (Ctx.E, C1 & " = cmp_lt_s.i32 " & To_String (Lo32) & ", " & To_String (AF_R));
                     Emit_Inst (Ctx.E, C2 & " = cmp_gt_s.i32 " & To_String (Hi32) & ", " & To_String (AL_R));
                     Emit_Inst (Ctx.E, Oob & " = or.i8 " & C1 & ", " & C2);
                     Emit_Inst (Ctx.E, Bad & " = and.i8 " & Non_Empty & ", " & Oob);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;

               declare
                  Off32 : constant String := Next_Reg (Ctx.E, "soff");
                  Off64 : constant String := Next_Reg (Ctx.E, "soff64");
                  SP    : constant String := Next_Reg (Ctx.E, "sptr");
               begin
                  Emit_Inst (Ctx.E, Off32 & " = sub.i32 " & To_String (Lo32) & ", " & To_String (AF_R));
                  Emit_Inst (Ctx.E, Off64 & " = sext.i64 " & Off32);
                  declare
                     SP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (Pfx_Val.Reg), Off64);
                  begin
                     Res.Kind           := EK_Slice;
                     Res.Reg            := To_Unbounded_String (SP2);
                  Res.Pir_Type       := To_Unbounded_String ("ptr");
                  Res.Ada_Type       := Base_Type_Spec (Arr_T);
                  Res.Align          := ETI.Align;
                  Res.NDims          := 1;
                  Res.First_Regs (1) := Lo32;
                  Res.Last_Regs (1)  := Hi32;
                  return Res;
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
               Inner : constant Expr_Val := Emit_LValue (Ctx, D (AS_EXP, Node));
            begin
               if ETI.Kind = TK_Constrained_Array or else ETI.Kind = TK_Unconstrained_Array then
                  declare
                     STI_C : constant Type_Info := Get_Type_Info (Ctx, Inner.Ada_Type);
                     ND : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, 4));
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
                           for D_I in 1 .. ND loop
                              declare
                                 NE : constant String := Next_Reg (Ctx.E, "cvne");
                                 L_Chk : constant String := Next_Label (Ctx.E, "cvc");
                                 L_Ok  : constant String := Next_Label (Ctx.E, "cvo");
                                 IT    : constant TREE := ETI.Dims (D_I).Idx_Type;
                                 ITI   : constant Type_Info := Get_Type_Info (Ctx, IT);
                              begin
                                 Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (SF (D_I)) & ", " & To_String (SL (D_I)));
                                 Emit_Term (Ctx.E, "br " & NE & ", " & L_Chk & ", " & L_Ok);
                                 Emit_Label (Ctx.E, L_Chk);
                                 Emit_Range_Check (Ctx, To_String (SF (D_I)), "i32", IT);
                                 Emit_Range_Check (Ctx, To_String (SL (D_I)), "i32", IT);
                                 Emit_Term (Ctx.E, "jmp " & L_Ok);
                                 Emit_Label (Ctx.E, L_Ok);
                              end;
                           end loop;
                        end if;
                        Res.First_Regs := SF;
                        Res.Last_Regs  := SL;
                     end if;
                     return Res;
                  end;
               elsif ETI.Kind = TK_Record then
                  Emit_Discriminant_Check (Ctx, To_String (Inner.Reg), ET, Is_Access => False);
                  Res := Inner;
                  Res.Ada_Type := ET;
                  return Res;
               else
                  return Emit_Expr (Ctx, Node);
               end if;
            end;

         when others =>
            return Emit_Expr (Ctx, Node);
      end case;
   end Emit_LValue;

   procedure Emit_Copy_Composite
     (Ctx      : in out Context;
      Dst_Ptr  : String;
      Src      : Expr_Val;
      Dst_Type : TREE)
   is
      DTI : constant Type_Info := Get_Type_Info (Ctx, Dst_Type);
   begin
      if DTI.Kind = TK_Record or else (DTI.Kind = TK_Constrained_Array and then not DTI.Is_Dynamic) then
         if DTI.Byte_Size > 0 then
            Emit_Inst (Ctx.E,
              "memmove " & Dst_Ptr & ", " & To_String (Src.Reg) & ", " &
              Img (DTI.Byte_Size) & ", align " & Img (DTI.Align));
         end if;
         -- Recursively deep-copy any embedded dynamic constrained array fields (@__ada_arr_desc)
         -- in records, nested records, active variants, and arrays of records (c52008b)
         declare
            function Type_Has_Dyn_Arr (T : TREE; Depth : Natural := 0) return Boolean is
               TI : Type_Info;
               BT : TREE;
               function CL_Has_Dyn (CL : TREE) return Boolean is
               begin
                  if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then
                     return False;
                  end if;
                  if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                     declare
                        DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                        DN : TREE;
                     begin
                        while not IS_EMPTY (DL) loop
                           POP (DL, DN);
                           if KIND (DN) = DN_VARIABLE_DECL then
                              declare
                                 NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DN));
                                 CID : TREE;
                              begin
                                 while not IS_EMPTY (NS) loop
                                    POP (NS, CID);
                                    if Type_Has_Dyn_Arr (Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID)), Depth + 1) then
                                       return True;
                                    end if;
                                 end loop;
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
                              if KIND (VN) = DN_VARIANT and then CL_Has_Dyn (D (AS_COMP_LIST, VN)) then
                                 return True;
                              end if;
                           end loop;
                        end;
                     end if;
                  end;
                  return False;
               end CL_Has_Dyn;
            begin
               if Depth > 16 or else Is_Void_Tree (T) then
                  return False;
               end if;
               TI := Get_Type_Info (Ctx, T);
               if TI.Kind = TK_Constrained_Array and then TI.Is_Dynamic then
                  return True;
               elsif TI.Kind = TK_Constrained_Array then
                  return Type_Has_Dyn_Arr (TI.Elem_Type, Depth + 1);
               elsif TI.Kind = TK_Record then
                  BT := Base_Type_Spec (Subst_Tree (Ctx, T));
                  if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
                     return CL_Has_Dyn (D (SM_COMP_LIST, BT));
                  end if;
               end if;
               return False;
            end Type_Has_Dyn_Arr;

            procedure Deep_Copy_Fields (Cur_Ptr : String; Cur_T : TREE) is
               CTI_Top : constant Type_Info := Get_Type_Info (Ctx, Cur_T);

               procedure Clone_Dyn_Arr (Arr_Desc_Ptr : String; Arr_T : TREE; ATI : Type_Info) is
                  ETI   : constant Type_Info := Get_Type_Info (Ctx, ATI.Elem_Type);
                  ESz   : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                  ND_A  : constant Natural := Natural'Max (1, Natural'Min (ATI.NDims, 4));
                  Orig  : Expr_Val;
                  Tot64 : Unbounded_String;
                  Byt   : constant String := Next_Reg (Ctx.E, "dcby");
                  Nz    : constant String := Next_Reg (Ctx.E, "dcnz");
                  AlB   : constant String := Next_Reg (Ctx.E, "dcal");
                  NewP  : Unbounded_String;
                  P0    : constant String := Next_Reg (Ctx.E, "dcp0");
               begin
                  Emit_Unpack_Arr_Desc (Ctx, Arr_Desc_Ptr, Arr_T, Orig);
                  for D_I in 1 .. ND_A loop
                     declare
                        CLen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Orig.First_Regs (D_I)), To_String (Orig.Last_Regs (D_I)));
                     begin
                        if D_I = 1 then Tot64 := To_Unbounded_String (CLen);
                        else
                           declare
                              M : constant String := Next_Reg (Ctx.E, "dcm");
                           begin
                              Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & CLen);
                              Tot64 := To_Unbounded_String (M);
                           end;
                        end if;
                     end;
                  end loop;
                  Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                  Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                  Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", NewP);
                  Emit_Inst (Ctx.E, "memcpy " & To_String (NewP) & ", " & To_String (Orig.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                  Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & Arr_Desc_Ptr & ", 0");
                  Emit_Inst (Ctx.E, "store.ptr " & To_String (NewP) & ", " & P0 & ", align 8");
               end Clone_Dyn_Arr;

               procedure Walk_Rec_CL (Rec_P : String; Rec_Pir : String; CL : TREE) is
               begin
                  if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then
                     return;
                  end if;
                  if not Is_Void_Tree (D (AS_DECL_S, CL)) then
                     declare
                        DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                        DN : TREE;
                     begin
                        while not IS_EMPTY (DL) loop
                           POP (DL, DN);
                           if KIND (DN) = DN_VARIABLE_DECL then
                              declare
                                 NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DN));
                                 CID : TREE;
                              begin
                                 while not IS_EMPTY (NS) loop
                                    POP (NS, CID);
                                    declare
                                       CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                                       CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                       CK  : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                    begin
                                       if Ctx.Fields.Contains (CK) and then Type_Has_Dyn_Arr (CT) then
                                          declare
                                             FE : constant Field_Entry := Ctx.Fields.Element (CK);
                                             FP : constant String := Next_Reg (Ctx.E, "dcfp");
                                          begin
                                             if FE.In_Union or else FE.Has_Explicit_Offset then
                                                declare
                                                   Off_R : constant String := Next_Reg (Ctx.E, "dcfo");
                                                begin
                                                   Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                                                   Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Rec_P & ", " & Off_R & " [inbounds]");
                                                end;
                                             else
                                                Emit_Inst (Ctx.E, FP & " = field_ptr " & Rec_Pir & ", " & Rec_P & ", " & Img (FE.Field_Idx));
                                             end if;
                                             if CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                                                Clone_Dyn_Arr (FP, CT, CTI);
                                             else
                                                Deep_Copy_Fields (FP, CT);
                                             end if;
                                          end;
                                       end if;
                                    end;
                                 end loop;
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
                           D_Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, VP))));
                           DK    : constant Unsigned_32 := Tree_Key (D_Def);
                        begin
                           if Ctx.Fields.Contains (DK) then
                              declare
                                 DFE   : constant Field_Entry := Ctx.Fields.Element (DK);
                                 DT_A  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, D_Def));
                                 DTI_D : constant Type_Info := Get_Type_Info (Ctx, DT_A);
                                 DP    : constant String := Next_Reg (Ctx.E, "dcdp");
                                 DV    : constant String := Next_Reg (Ctx.E, "dcdv");
                                 D64   : Unbounded_String;
                                 VL    : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                                 VN    : TREE;
                                 L_End : constant String := Next_Label (Ctx.E, "dcve");
                              begin
                                 Emit_Inst (Ctx.E, DP & " = field_ptr " & Rec_Pir & ", " & Rec_P & ", " & Img (DFE.Field_Idx));
                                 Emit_Inst (Ctx.E, DV & " = load." & To_String (DTI_D.Pir_Type) & " " & DP & ", align " & Img (Natural'Max (1, DTI_D.Align)));
                                 D64 := To_Unbounded_String (Emit_Int_Convert (Ctx, DV, To_String (DTI_D.Pir_Type), "i64"));
                                 while not IS_EMPTY (VL) loop
                                    POP (VL, VN);
                                    if KIND (VN) = DN_VARIANT then
                                       declare
                                          CS    : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN));
                                          Ch    : TREE;
                                          Cond  : Unbounded_String;
                                          Is_Ot : Boolean := False;
                                          L_Hit : constant String := Next_Label (Ctx.E, "dcvh");
                                          L_Nxt : constant String := Next_Label (Ctx.E, "dcvn");
                                       begin
                                          while not IS_EMPTY (CS) loop
                                             POP (CS, Ch);
                                             if KIND (Ch) = DN_CHOICE_OTHERS then
                                                Is_Ot := True;
                                             elsif KIND (Ch) = DN_CHOICE_EXP then
                                                declare
                                                   CE  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch), DT_A);
                                                   C64 : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i64");
                                                   Eq  : constant String := Next_Reg (Ctx.E, "dcce");
                                                begin
                                                   Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & To_String (D64) & ", " & C64);
                                                   if Length (Cond) = 0 then Cond := To_Unbounded_String (Eq);
                                                   else
                                                      declare
                                                         O2 : constant String := Next_Reg (Ctx.E, "dcco");
                                                      begin
                                                         Emit_Inst (Ctx.E, O2 & " = or.i8 " & To_String (Cond) & ", " & Eq);
                                                         Cond := To_Unbounded_String (O2);
                                                      end;
                                                   end if;
                                                end;
                                             elsif KIND (Ch) = DN_CHOICE_RANGE then
                                                declare
                                                   RT2, RL, RH : Unbounded_String;
                                                   L64, H64 : Unbounded_String;
                                                   G1  : constant String := Next_Reg (Ctx.E, "dccg");
                                                   L1  : constant String := Next_Reg (Ctx.E, "dccl");
                                                   InR : constant String := Next_Reg (Ctx.E, "dccr");
                                                begin
                                                   Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT2, RL, RH);
                                                   L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT2), "i64"));
                                                   H64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT2), "i64"));
                                                   Emit_Inst (Ctx.E, G1 & " = cmp_ge_s.i64 " & To_String (D64) & ", " & To_String (L64));
                                                   Emit_Inst (Ctx.E, L1 & " = cmp_le_s.i64 " & To_String (D64) & ", " & To_String (H64));
                                                   Emit_Inst (Ctx.E, InR & " = and.i8 " & G1 & ", " & L1);
                                                   if Length (Cond) = 0 then Cond := To_Unbounded_String (InR);
                                                   else
                                                      declare
                                                         O2 : constant String := Next_Reg (Ctx.E, "dcco");
                                                      begin
                                                         Emit_Inst (Ctx.E, O2 & " = or.i8 " & To_String (Cond) & ", " & InR);
                                                         Cond := To_Unbounded_String (O2);
                                                      end;
                                                   end if;
                                                end;
                                             end if;
                                          end loop;
                                          if Is_Ot then
                                             Emit_Term (Ctx.E, "jmp " & L_Hit);
                                          elsif Length (Cond) > 0 then
                                             Emit_Term (Ctx.E, "br " & To_String (Cond) & ", " & L_Hit & ", " & L_Nxt);
                                          else
                                             Emit_Term (Ctx.E, "jmp " & L_Nxt);
                                          end if;
                                          Emit_Label (Ctx.E, L_Hit);
                                          Walk_Rec_CL (Rec_P, Rec_Pir, D (AS_COMP_LIST, VN));
                                          Emit_Term (Ctx.E, "jmp " & L_End);
                                          Emit_Label (Ctx.E, L_Nxt);
                                       end;
                                    end if;
                                 end loop;
                                 Emit_Term (Ctx.E, "jmp " & L_End);
                                 Emit_Label (Ctx.E, L_End);
                              end;
                           end if;
                        end;
                     end if;
                  end;
               end Walk_Rec_CL;
            begin
               if CTI_Top.Kind = TK_Record then
                  declare
                     BT : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, Cur_T));
                  begin
                     if not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
                        Walk_Rec_CL (Cur_Ptr, To_String (CTI_Top.Pir_Type), D (SM_COMP_LIST, BT));
                     end if;
                  end;
               elsif CTI_Top.Kind = TK_Constrained_Array and then not CTI_Top.Is_Dynamic then
                  declare
                     ETI : constant Type_Info := Get_Type_Info (Ctx, CTI_Top.Elem_Type);
                     EAl : constant Natural := Natural'Max (1, ETI.Align);
                     ESz : constant Natural := Natural'Max (1, ((ETI.Byte_Size + EAl - 1) / EAl) * EAl);
                     Total_Elems : Natural := 1;
                     Zero64 : constant String := Next_Reg (Ctx.E, "dc0");
                     L_Hdr  : constant String := Next_Label (Ctx.E, "dch");
                     L_Body : constant String := Next_Label (Ctx.E, "dcb");
                     L_End  : constant String := Next_Label (Ctx.E, "dce");
                     Idx_R  : constant String := Next_Reg (Ctx.E, "dci");
                     Cmp_R  : constant String := Next_Reg (Ctx.E, "dcc");
                     Off_R  : constant String := Next_Reg (Ctx.E, "dco");
                     EP_R   : constant String := Next_Reg (Ctx.E, "dcep");
                     Nxt_R  : constant String := Next_Reg (Ctx.E, "dcn");
                  begin
                     for D_I in 1 .. CTI_Top.NDims loop
                        if CTI_Top.Dims (D_I).Last >= CTI_Top.Dims (D_I).First then
                           Total_Elems := Total_Elems * Natural (CTI_Top.Dims (D_I).Last - CTI_Top.Dims (D_I).First + 1);
                        else
                           Total_Elems := 0;
                        end if;
                     end loop;
                     if Total_Elems > 0 then
                        Emit_Inst (Ctx.E, Zero64 & " = const.i64 0");
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Zero64 & ")");
                        Emit_Label (Ctx.E, L_Hdr, Idx_R & ": i64");
                        Emit_Inst (Ctx.E, Cmp_R & " = cmp_lt_s.i64 " & Idx_R & ", " & Img (Total_Elems));
                        Emit_Term (Ctx.E, "br " & Cmp_R & ", " & L_Body & ", " & L_End);
                        Emit_Label (Ctx.E, L_Body);
                        Emit_Inst (Ctx.E, Off_R & " = mul.i64 " & Idx_R & ", " & Img (ESz));
                        Emit_Inst (Ctx.E, EP_R & " = ptr_add.i64 " & Cur_Ptr & ", " & Off_R & " [inbounds]");
                        Deep_Copy_Fields (EP_R, CTI_Top.Elem_Type);
                        Emit_Inst (Ctx.E, Nxt_R & " = add.i64 " & Idx_R & ", 1");
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nxt_R & ")");
                        Emit_Label (Ctx.E, L_End);
                     end if;
                  end;
               end if;
            end Deep_Copy_Fields;
         begin
            if Type_Has_Dyn_Arr (Dst_Type) then
               Deep_Copy_Fields (Dst_Ptr, Dst_Type);
            end if;
         end;
      elsif DTI.Kind = TK_Unconstrained_Array or else DTI.Is_Dynamic then
         -- Copy based on Src's dynamic length across all dimensions
         declare
            ETI   : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
            Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
            EAl   : constant Natural := (if Is_DE then 8 else Natural'Max (1, ETI.Align));
            ND    : constant Natural := (if DTI.NDims > 0 then DTI.NDims else 1);
            Tot64 : Unbounded_String;
            Byt   : constant String := Next_Reg (Ctx.E, "cby");
            ESz   : constant Natural := (if Is_DE then 40 else ((ETI.Byte_Size + EAl - 1) / EAl) * EAl);
         begin
            for D_Idx in 1 .. ND loop
               declare
                  F, L : Unbounded_String;
                  L64  : Unbounded_String;
               begin
                  Get_Array_Dim_Bounds (Ctx, Src, D_Idx, F, L);
                  L64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F), To_String (L)));
                  if D_Idx = 1 then
                     Tot64 := L64;
                  else
                     declare
                        M : constant String := Next_Reg (Ctx.E, "cm");
                     begin
                        Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & To_String (L64));
                        Tot64 := To_Unbounded_String (M);
                     end;
                  end if;
               end;
            end loop;
            Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
            Emit_Inst (Ctx.E, "memmove " & Dst_Ptr & ", " & To_String (Src.Reg) & ", " & Byt & ", align " & Img (EAl));
         end;
      end if;
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
      DTI : constant Type_Info := Get_Type_Info (Ctx, Dst_Type);
   begin
      if Is_Void_Tree (Init_Node) then
         return;
      end if;
      if KIND (Init_Node) = DN_QUALIFIED then
         declare
            Qual_T : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Init_Node));
            QTI    : constant Type_Info := (if not Is_Void_Tree (Qual_T) then Get_Type_Info (Ctx, Qual_T) else DTI);
         begin
            Emit_Init_Composite (Ctx, Dst_Ptr, Dst_Type, D (AS_EXP, Init_Node), Dst_Firsts, Dst_Lasts, Exact_Bounds => True);
            if not Is_Void_Tree (Qual_T) and then QTI.Kind = TK_Record then
               Emit_Discriminant_Check (Ctx, Dst_Ptr, Qual_T, Is_Access => False);
            end if;
         end;
         return;
      end if;
      if KIND (Init_Node) = DN_PARENTHESIZED then
         if DTI.Kind = TK_Constrained_Array
           and then not Is_Void_Tree (D (AS_EXP, Init_Node))
           and then KIND (D (AS_EXP, Init_Node)) in DN_AGGREGATE | DN_STRING_LITERAL
         then
            -- LRM 4.3.2(5)+(9) (c43205f): parenthesized aggregate loses applicable index constraint
            -- (lower bound comes from base index subtype 'FIRST), and as an aggregate component must match Dst_Type bounds!
            declare
               Base_Arr : constant TREE := Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type)));
               Src      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Init_Node), Base_Arr);
               ND_A     : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, 4));
            begin
               if Ctx.Enable_Checks then
                  for D_I in 1 .. ND_A loop
                     declare
                        SF, SL, DF, DL : Unbounded_String;
                        C1  : constant String := Next_Reg (Ctx.E, "pab1");
                        C2  : constant String := Next_Reg (Ctx.E, "pab2");
                        Bad : constant String := Next_Reg (Ctx.E, "pabb");
                     begin
                        Get_Array_Dim_Bounds (Ctx, Src, D_I, SF, SL);
                        if Length (Dst_Firsts (D_I)) > 0 then
                           DF := Dst_Firsts (D_I);
                           DL := Dst_Lasts (D_I);
                        else
                           Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, DF, DL);
                        end if;
                        Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (SF) & ", " & To_String (DF));
                        Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (SL) & ", " & To_String (DL));
                        Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                        Emit_Check_Fail_If (Ctx, Bad);
                     end;
                  end loop;
               end if;
               Emit_Copy_Composite (Ctx, Dst_Ptr, Src, Dst_Type);
            end;
            return;
         end if;
         Emit_Init_Composite (Ctx, Dst_Ptr, Dst_Type, D (AS_EXP, Init_Node), Dst_Firsts, Dst_Lasts, Exact_Bounds => Exact_Bounds);
         return;
      end if;

      if KIND (Init_Node) = DN_AGGREGATE then
         if DTI.Kind = TK_Record then
            declare
               Norm     : TREE := D (SM_NORMALIZED_COMP_S, Init_Node);
               Comps    : SEQ_TYPE;
               C_Exp    : TREE;
               Base_Rec : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, Dst_Type));
               -- Collect component IDs in field order (discriminants + fixed + active variant)
               Comp_Ids    : array (0 .. 255) of TREE;
               N_Comps     : Natural := 0;
               Disc_Vals   : array (0 .. 31) of Integer_64 := (others => 0);
               Disc_Static : array (0 .. 31) of Boolean := (others => False);
               Disc_Ids    : array (0 .. 31) of TREE := (others => CONST_VOID);
               N_Discs     : Natural := 0;
               Bound_Discs : U32_Vecs.Vector;

               procedure Gather_Ids (DS : TREE; Is_Disc : Boolean := False) is
                  DL  : SEQ_TYPE;
                  D_N : TREE;
               begin
                  if Is_Void_Tree (DS) then return; end if;
                  DL := LIST (DS);
                  while not IS_EMPTY (DL) loop
                     POP (DL, D_N);
                     if KIND (D_N) = DN_VARIABLE_DECL or else KIND (D_N) = DN_DSCRMT_DECL then
                        declare
                           NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, D_N));
                           ID : TREE;
                        begin
                           while not IS_EMPTY (NS) loop
                              POP (NS, ID);
                              if N_Comps < 256 then
                                 Comp_Ids (N_Comps) := ID;
                                 N_Comps := N_Comps + 1;
                              end if;
                              if Is_Disc and then N_Discs < 32 then
                                 Disc_Ids (N_Discs) := ID;
                                 N_Discs := N_Discs + 1;
                              end if;
                           end loop;
                        end;
                     end if;
                  end loop;
               end Gather_Ids;

               function Match_Variant_Choice (Ch : TREE; Val : Integer_64) return Boolean is
               begin
                  if KIND (Ch) = DN_CHOICE_OTHERS then
                     return True;
                  elsif KIND (Ch) = DN_CHOICE_EXP then
                     declare
                        SV : constant TREE := Safe_SM_Value (D (AS_EXP, Ch));
                     begin
                        return Has_Static_Val (SV) and then Eval_Int64 (SV) = Val;
                     end;
                  elsif KIND (Ch) = DN_CHOICE_RANGE then
                     declare
                        DR : constant TREE := D (AS_DISCRETE_RANGE, Ch);
                     begin
                        if not Is_Void_Tree (DR) and then KIND (DR) = DN_RANGE then
                           declare
                              V1 : constant TREE := Safe_SM_Value (D (AS_EXP1, DR));
                              V2 : constant TREE := Safe_SM_Value (D (AS_EXP2, DR));
                           begin
                              return Has_Static_Val (V1) and then Has_Static_Val (V2)
                                and then Val >= Eval_Int64 (V1) and then Val <= Eval_Int64 (V2);
                           end;
                        elsif not Is_Void_Tree (DR) and then KIND (DR) = DN_DISCRETE_SUBTYPE then
                           declare
                              St : Boolean := False;
                              Lo, Hi : Integer_64 := 0;
                              Sub_N : constant TREE := D (AS_SUBTYPE_INDICATION, DR);
                              TM    : constant TREE := (if not Is_Void_Tree (Sub_N) then Get_Name_Defn (D (AS_NAME, Sub_N)) else CONST_VOID);
                           begin
                              if not Is_Void_Tree (TM) and then KIND (TM) in CLASS_TYPE_NAME then
                                 Get_Discrete_Bounds (D (SM_TYPE_SPEC, TM), St, Lo, Hi);
                                 return St and then Val >= Lo and then Val <= Hi;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
                  return False;
               end Match_Variant_Choice;

               procedure Gather_Comp_List (CL : TREE) is
               begin
                  if Is_Void_Tree (CL) then return; end if;
                  Gather_Ids (D (AS_DECL_S, CL));
                  declare
                     VP : constant TREE := D (AS_VARIANT_PART, CL);
                  begin
                     if not Is_Void_Tree (VP) and then KIND (VP) = DN_VARIANT_PART then
                        declare
                           DN_Name : constant TREE := D (AS_NAME, VP);
                           D_Def   : constant TREE := Canonical_Def (Get_Name_Defn (DN_Name));
                           D_Val   : Integer_64 := 0;
                           Has_Val : Boolean := False;
                           VS      : constant TREE := D (AS_VARIANT_S, VP);
                           VL      : SEQ_TYPE := (if not Is_Void_Tree (VS) then LIST (VS) else MAKE);
                           VN      : TREE;
                           Matched : TREE := CONST_VOID;
                           Others_V : TREE := CONST_VOID;
                        begin
                           for I in 0 .. N_Discs - 1 loop
                              if Canonical_Def (Disc_Ids (I)) = D_Def and then Disc_Static (I) then
                                 D_Val := Disc_Vals (I);
                                 Has_Val := True;
                                 exit;
                              end if;
                           end loop;
                           while not IS_EMPTY (VL) loop
                              POP (VL, VN);
                              if KIND (VN) = DN_VARIANT then
                                 declare
                                    CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN));
                                    Ch : TREE;
                                 begin
                                    while not IS_EMPTY (CS) loop
                                       POP (CS, Ch);
                                       if KIND (Ch) = DN_CHOICE_OTHERS then
                                          Others_V := VN;
                                       elsif Has_Val and then Match_Variant_Choice (Ch, D_Val) then
                                          Matched := VN;
                                          exit;
                                       end if;
                                    end loop;
                                 end;
                                 exit when not Is_Void_Tree (Matched);
                              end if;
                           end loop;
                           if Is_Void_Tree (Matched) then
                              Matched := Others_V;
                           end if;
                           if not Is_Void_Tree (Matched) then
                              Gather_Comp_List (D (AS_COMP_LIST, Matched));
                           end if;
                        end;
                     end if;
                  end;
               end Gather_Comp_List;

               function Emit_Field_Ptr_For (CID : TREE; Fallback_Idx : Natural) return String is
                  CK : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                  FP : constant String := Next_Reg (Ctx.E, "afp");
               begin
                  if Ctx.Fields.Contains (CK) then
                     declare
                        FE : constant Field_Entry := Ctx.Fields.Element (CK);
                     begin
                        if not FE.In_Union and then not FE.Has_Explicit_Offset then
                           Emit_Inst (Ctx.E,
                             FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " & Dst_Ptr & ", " & Img (FE.Field_Idx));
                        else
                           declare
                              Off_R : constant String := Next_Reg (Ctx.E, "afo");
                           begin
                              Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                              Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                           end;
                        end if;
                     end;
                  else
                     Emit_Inst (Ctx.E,
                       FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " & Dst_Ptr & ", " & Img (Fallback_Idx));
                  end if;
                  return FP;
               end Emit_Field_Ptr_For;
            begin
               if KIND (Base_Rec) /= DN_RECORD then
                  return;
               end if;
               if Is_Void_Tree (Norm) then
                  Norm := D (AS_GENERAL_ASSOC_S, Init_Node);
               end if;
               Gather_Ids (D (SM_DISCRIMINANT_S, Base_Rec), Is_Disc => True);
               -- Peek static discriminant values from Norm so Gather_Comp_List selects the active variant!
               declare
                  Peek : SEQ_TYPE := LIST (Norm);
                  PE   : TREE;
                  DI_I : Natural := 0;
               begin
                  while not IS_EMPTY (Peek) and then DI_I < N_Discs loop
                     POP (Peek, PE);
                     if KIND (PE) = DN_ASSOC or else KIND (PE) = DN_NAMED then
                        PE := D (AS_EXP, PE);
                     end if;
                     if not Is_Void_Tree (PE) and then Has_Static_Val (Safe_SM_Value (PE)) then
                        Disc_Vals (DI_I) := Eval_Int64 (Safe_SM_Value (PE));
                        Disc_Static (DI_I) := True;
                     end if;
                     DI_I := DI_I + 1;
                  end loop;
               end;
               if not Is_Void_Tree (D (SM_COMP_LIST, Base_Rec)) then
                  Gather_Comp_List (D (SM_COMP_LIST, Base_Rec));
               end if;
               -- Zero-init record buffer first so padding/inactive union bytes are deterministic
               if DTI.Byte_Size > 0 then
                  Emit_Inst (Ctx.E, "memset " & Dst_Ptr & ", 0, " & Img (DTI.Byte_Size) & ", align " & Img (Natural'Max (1, DTI.Align)));
               end if;
               Comps := LIST (Norm);
               declare
                  Idx : Natural := 0;
               begin
                  while not IS_EMPTY (Comps) and then Idx < N_Comps loop
                     POP (Comps, C_Exp);
                     if KIND (C_Exp) = DN_ASSOC or else KIND (C_Exp) = DN_NAMED then
                        C_Exp := D (AS_EXP, C_Exp);
                     end if;
                     declare
                        CID : constant TREE := Comp_Ids (Idx);
                        CID_K : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                        CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                        RT_C : constant TREE := Resolve_Type_Spec (CT);
                        CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                        FP  : constant String := Emit_Field_Ptr_For (CID, Idx);
                        Has_SD_Save : Boolean := False;
                        Prev_SD     : Saved_Disc_Info;
                        Has_SR_Save : Boolean := False;
                        Prev_SR     : Saved_Range_Info;
                        SR_Key      : Unsigned_32 := 0;
                     begin
                        if not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Discs.Contains (CID_K) then
                           if Ctx.Saved_Type_Discs.Contains (Tree_Key (RT_C)) then
                              Has_SD_Save := True;
                              Prev_SD := Ctx.Saved_Type_Discs.Element (Tree_Key (RT_C));
                           end if;
                           Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Ctx.Saved_Type_Discs.Element (CID_K));
                        elsif not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Ranges.Contains (CID_K) then
                           if KIND (RT_C) = DN_CONSTRAINED_ARRAY and then CTI.NDims >= 1 and then not Is_Void_Tree (CTI.Dims (1).Idx_Type) then
                              SR_Key := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, CTI.Dims (1).Idx_Type)));
                           else
                              SR_Key := Tree_Key (RT_C);
                           end if;
                           if Ctx.Saved_Type_Ranges.Contains (SR_Key) then
                              Has_SR_Save := True;
                              Prev_SR := Ctx.Saved_Type_Ranges.Element (SR_Key);
                           end if;
                           Ctx.Saved_Type_Ranges.Include (SR_Key, Ctx.Saved_Type_Ranges.Element (CID_K));
                        end if;
                        if CTI.Kind = TK_Scalar then
                           declare
                              V   : constant Expr_Val := Emit_Expr (Ctx, C_Exp, CT);
                              VR  : constant String :=
                                Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (CTI.Pir_Type));
                              CK_F : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                           begin
                              Emit_Range_Check (Ctx, VR, To_String (CTI.Pir_Type), CT);
                              Emit_Discriminant_Check (Ctx, VR, CT, Is_Access => True);
                              if Ctx.Fields.Contains (CK_F) and then Is_Bitfield (Ctx.Fields.Element (CK_F)) then
                                 Emit_Bitfield_Store (Ctx, Dst_Ptr, Ctx.Fields.Element (CK_F), VR, To_String (CTI.Pir_Type), CT);
                              else
                                 Emit_Inst (Ctx.E,
                                   "store." & To_String (CTI.Pir_Type) & " " & VR &
                                   ", " & FP & ", align " & Img (CTI.Align));
                              end if;
                           end;
                           -- If this is a discriminant, bind it in Ctx.Vars for subsequent dynamic-array components!
                           if Idx < N_Discs then
                              declare
                                 DK : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                 DV : Var_Info;
                              begin
                                 DV.Kind       := VK_Local_Alloca;
                                 DV.Pir_Name   := To_Unbounded_String (FP);
                                 DV.Pir_Type   := CTI.Pir_Type;
                                 DV.Ada_Type   := CT;
                                 DV.Align      := CTI.Align;
                                 DV.Owner_Subp := Ctx.Cur_Subp_Key;
                                 Ctx.Vars.Include (DK, DV);
                                 Bound_Discs.Append (DK);
                              end;
                           end if;
                        elsif CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                           -- Embedded @__ada_arr_desc for dynamic constrained array field (RC-8)
                           declare
                              ETI   : constant Type_Info := Get_Type_Info (Ctx, CTI.Elem_Type);
                              ESz   : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                              ND_A  : constant Natural := Natural'Max (1, Natural'Min (CTI.NDims, 4));
                              Firsts, Lasts : Str_Array_4;
                              Tot64 : Unbounded_String;
                              Byt   : constant String := Next_Reg (Ctx.E, "raby");
                              Nz    : constant String := Next_Reg (Ctx.E, "ranz");
                              AlB   : constant String := Next_Reg (Ctx.E, "raal");
                              DataP : Unbounded_String;
                              P0    : constant String := Next_Reg (Ctx.E, "rap0");
                           begin
                              for D_I in 1 .. ND_A loop
                                 Emit_Index_Bounds (Ctx, CTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                                 if Ctx.Enable_Checks then
                                    declare
                                       BA  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (CT));
                                       BTI : constant Type_Info := (if not Is_Void_Tree (BA) and then KIND (BA) = DN_ARRAY then Get_Type_Info (Ctx, BA) else CTI);
                                    begin
                                       if D_I <= BTI.NDims and then not Is_Void_Tree (BTI.Dims (D_I).Idx_Type) then
                                          declare
                                             NE  : constant String := Next_Reg (Ctx.E, "rane");
                                             L_C : constant String := Next_Label (Ctx.E, "rac");
                                             L_O : constant String := Next_Label (Ctx.E, "rao");
                                             BIT : constant TREE := BTI.Dims (D_I).Idx_Type;
                                             BITI : constant Type_Info := Get_Type_Info (Ctx, BIT);
                                          begin
                                             Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (Firsts (D_I)) & ", " & To_String (Lasts (D_I)));
                                             Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                                             Emit_Label (Ctx.E, L_C);
                                             declare
                                                FC : constant String := Emit_Int_Convert (Ctx, To_String (Firsts (D_I)), "i32", To_String (BITI.Pir_Type));
                                                LC : constant String := Emit_Int_Convert (Ctx, To_String (Lasts (D_I)), "i32", To_String (BITI.Pir_Type));
                                             begin
                                                Emit_Range_Check (Ctx, FC, To_String (BITI.Pir_Type), BIT);
                                                Emit_Range_Check (Ctx, LC, To_String (BITI.Pir_Type), BIT);
                                             end;
                                             Emit_Term (Ctx.E, "jmp " & L_O);
                                             Emit_Label (Ctx.E, L_O);
                                          end;
                                       end if;
                                    end;
                                 end if;
                                 declare
                                    DL : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)));
                                 begin
                                    if D_I = 1 then Tot64 := To_Unbounded_String (DL);
                                    else
                                       declare
                                          M : constant String := Next_Reg (Ctx.E, "ram");
                                       begin
                                          Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & DL);
                                          Tot64 := To_Unbounded_String (M);
                                       end;
                                    end if;
                                 end;
                              end loop;
                              Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                              Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                              Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                              Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
                              Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & FP & ", 0");
                              Emit_Inst (Ctx.E, "store.ptr " & To_String (DataP) & ", " & P0 & ", align 8");
                              for D_I in 1 .. ND_A loop
                                 declare
                                    FF : constant String := Next_Reg (Ctx.E, "raff");
                                    LF : constant String := Next_Reg (Ctx.E, "ralf");
                                 begin
                                    Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & FP & ", " & Img (2 * D_I - 1));
                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (Firsts (D_I)) & ", " & FF & ", align 4");
                                    Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & FP & ", " & Img (2 * D_I));
                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (Lasts (D_I)) & ", " & LF & ", align 4");
                                 end;
                              end loop;
                              if KIND (C_Exp) = DN_AGGREGATE then
                                 declare
                                    function Agg_All_Named_No_Others (AN : TREE; Rem_D : Natural) return Boolean is
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
                                             if KIND (IT) not in DN_ASSOC | DN_NAMED then
                                                return False;
                                             end if;
                                             declare
                                                CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                                                Ch : TREE;
                                             begin
                                                while not IS_EMPTY (CS) loop
                                                   POP (CS, Ch);
                                                   if KIND (Ch) = DN_CHOICE_OTHERS then return False; end if;
                                                end loop;
                                             end;
                                             if Rem_D > 1 and then not Agg_All_Named_No_Others (D (AS_EXP, IT), Rem_D - 1) then
                                                return False;
                                             end if;
                                          end loop;
                                          return Has_Any;
                                       end;
                                    end Agg_All_Named_No_Others;
                                 begin
                                    if Agg_All_Named_No_Others (C_Exp, ND_A) then
                                       -- LRM 4.3.1(3) + 4.3.2(8) (c43103b Case 5): evaluate all dim choices via Pass-1 and check exact bounds!
                                       declare
                                          Base_CT : constant TREE := Subst_Base_Type (Ctx, CT);
                                          SV      : constant Expr_Val := Emit_Expr (Ctx, C_Exp, Base_CT);
                                       begin
                                          if Ctx.Enable_Checks then
                                             for D_I in 1 .. ND_A loop
                                                declare
                                                   SF, SL : Unbounded_String;
                                                   C1  : constant String := Next_Reg (Ctx.E, "ran1");
                                                   C2  : constant String := Next_Reg (Ctx.E, "ran2");
                                                   Bad : constant String := Next_Reg (Ctx.E, "ranb");
                                                begin
                                                   Get_Array_Dim_Bounds (Ctx, SV, D_I, SF, SL);
                                                   Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (SF) & ", " & To_String (Firsts (D_I)));
                                                   Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (SL) & ", " & To_String (Lasts (D_I)));
                                                   Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                                   Emit_Check_Fail_If (Ctx, Bad);
                                                end;
                                             end loop;
                                          end if;
                                          Emit_Inst (Ctx.E, "memmove " & To_String (DataP) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                                       end;
                                    else
                                       Emit_Init_Composite (Ctx, To_String (DataP), CT, C_Exp, Firsts, Lasts, Exact_Bounds => True);
                                    end if;
                                 end;
                              else
                                 declare
                                    Is_Par : constant Boolean := KIND (C_Exp) = DN_PARENTHESIZED;
                                    Eval_T : constant TREE :=
                                      (if Is_Par then Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, CT))) else CT);
                                    SV  : constant Expr_Val := Emit_Expr (Ctx, C_Exp, Eval_T);
                                    Tgt : Expr_Val;
                                 begin
                                    Tgt.Kind := EK_Slice; Tgt.Reg := DataP; Tgt.Ada_Type := CT;
                                    Tgt.NDims := ND_A; Tgt.First_Regs := Firsts; Tgt.Last_Regs := Lasts;
                                    if not Is_Par and then KIND (C_Exp) = DN_STRING_LITERAL then
                                       Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                                    elsif Ctx.Enable_Checks then
                                       -- LRM 4.3.1(3) (c43004b, c43103b): record aggregate constrained array component requires EXACT bounds (no sliding)!
                                       for D_I in 1 .. ND_A loop
                                          declare
                                             SF, SL : Unbounded_String;
                                             C1  : constant String := Next_Reg (Ctx.E, "rab1");
                                             C2  : constant String := Next_Reg (Ctx.E, "rab2");
                                             Bad : constant String := Next_Reg (Ctx.E, "rabb");
                                          begin
                                             Get_Array_Dim_Bounds (Ctx, SV, D_I, SF, SL);
                                             Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (SF) & ", " & To_String (Firsts (D_I)));
                                             Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (SL) & ", " & To_String (Lasts (D_I)));
                                             Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                             Emit_Check_Fail_If (Ctx, Bad);
                                          end;
                                       end loop;
                                    end if;
                                    Emit_Inst (Ctx.E, "memmove " & To_String (DataP) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                                 end;
                              end if;
                           end;
                        else
                           declare
                              Need_Exact : constant Boolean :=
                                CTI.Kind = TK_Constrained_Array
                                and then KIND (C_Exp) not in DN_AGGREGATE | DN_STRING_LITERAL;
                           begin
                              Emit_Init_Composite (Ctx, FP, CT, C_Exp, Exact_Bounds => Need_Exact);
                           end;
                           if CTI.Kind = TK_Record then
                              Emit_Discriminant_Check (Ctx, FP, CT, Is_Access => False);
                           end if;
                        end if;
                        if Has_SD_Save and then not Is_Void_Tree (RT_C) then
                           Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Prev_SD);
                        end if;
                        if Has_SR_Save and then SR_Key /= 0 then
                           Ctx.Saved_Type_Ranges.Include (SR_Key, Prev_SR);
                        end if;
                     end;
                     Idx := Idx + 1;
                  end loop;
               end;
               for DK of Bound_Discs loop
                  if Ctx.Vars.Contains (DK) then
                     Ctx.Vars.Delete (DK);
                  end if;
               end loop;
            end;
            return;

         elsif DTI.Kind = TK_Constrained_Array or else DTI.Kind = TK_Unconstrained_Array then
            declare
               ETI    : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
               ND_Tot : constant Natural := Natural'Max (1, DTI.NDims);

               procedure Get_Dim_Lo_Hi
                 (Dim : Natural;
                  Lo  : out Unbounded_String;
                  Hi  : out Unbounded_String)
               is
               begin
                  if Dim <= 4 and then Length (Dst_Firsts (Dim)) > 0 then
                     Lo := Dst_Firsts (Dim);
                     Hi := Dst_Lasts (Dim);
                  elsif DTI.Kind = TK_Constrained_Array and then Dim <= DTI.NDims and then DTI.Dims (Dim).Is_Static then
                     declare
                        RF : constant String := Next_Reg (Ctx.E, "af");
                        RL : constant String := Next_Reg (Ctx.E, "al");
                     begin
                        Emit_Inst (Ctx.E, RF & " = const.i32 " & Img (Long_Long_Integer (DTI.Dims (Dim).First)));
                        Emit_Inst (Ctx.E, RL & " = const.i32 " & Img (Long_Long_Integer (DTI.Dims (Dim).Last)));
                        Lo := To_Unbounded_String (RF);
                        Hi := To_Unbounded_String (RL);
                     end;
                  else
                     declare
                        RF : constant String := Next_Reg (Ctx.E, "af");
                        RL : constant String := Next_Reg (Ctx.E, "al");
                     begin
                        Emit_Inst (Ctx.E, RF & " = const.i32 1");
                        Emit_Inst (Ctx.E, RL & " = const.i32 0");
                        Lo := To_Unbounded_String (RF);
                        Hi := To_Unbounded_String (RL);
                     end;
                  end if;
               end Get_Dim_Lo_Hi;

               function Compute_Sub_Stride (Dim : Natural) return String is
                  Acc : Unbounded_String;
                  One : constant String := Next_Reg (Ctx.E, "st1");
               begin
                  Emit_Inst (Ctx.E, One & " = const.i64 1");
                  Acc := To_Unbounded_String (One);
                  for D_I in Dim + 1 .. ND_Tot loop
                     declare
                        FL, FH : Unbounded_String;
                        Sp : constant String := Next_Reg (Ctx.E, "ssp");
                        Ln : constant String := Next_Reg (Ctx.E, "sln");
                        Ps : constant String := Next_Reg (Ctx.E, "sps");
                        Cl : constant String := Next_Reg (Ctx.E, "scl");
                        C6 : constant String := Next_Reg (Ctx.E, "sc6");
                        Nx : constant String := Next_Reg (Ctx.E, "sm");
                     begin
                        Get_Dim_Lo_Hi (D_I, FL, FH);
                        Emit_Inst (Ctx.E, Sp & " = sub.i32 " & To_String (FH) & ", " & To_String (FL));
                        Emit_Inst (Ctx.E, Ln & " = add.i32 " & Sp & ", 1");
                        Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                        Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                        Emit_Inst (Ctx.E, C6 & " = sext.i64 " & Cl);
                        Emit_Inst (Ctx.E, Nx & " = mul.i64 " & To_String (Acc) & ", " & C6);
                        Acc := To_Unbounded_String (Nx);
                     end;
                  end loop;
                  return To_String (Acc);
               end Compute_Sub_Stride;

               procedure Emit_Init_Array_Dim
                 (Cur_Ptr  : String;
                  Agg_Node : TREE;
                  Dim      : Natural)
               is
                  Assocs : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, Agg_Node));
                  Item   : TREE;
                  Pos    : Integer_64 := 0;
                  Lo_Reg : Unbounded_String;
                  Hi_Reg : Unbounded_String;
                  Stride : constant String := (if Dim < ND_Tot then Compute_Sub_Stride (Dim) else "");

                  procedure Store_Elem_At (Off64 : String; Val_Node : TREE) is
                     EP : constant String := Next_Reg (Ctx.E, "aep");
                  begin
                     if Dim < ND_Tot then
                        declare
                           Scaled : constant String := Next_Reg (Ctx.E, "asc");
                           EP_S   : Unbounded_String;
                        begin
                           Emit_Inst (Ctx.E, Scaled & " = mul.i64 " & Off64 & ", " & Stride);
                           EP_S := To_Unbounded_String (Emit_Elem_Ptr (Ctx, ETI, Cur_Ptr, Scaled));
                           if not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_AGGREGATE then
                              Emit_Init_Array_Dim (To_String (EP_S), Val_Node, Dim + 1);
                           elsif not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_STRING_LITERAL then
                              declare
                                 SV  : constant Expr_Val := Emit_Expr (Ctx, Val_Node);
                                 ESz : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                                 Byt : constant String := Next_Reg (Ctx.E, "sby");
                              begin
                                 Emit_Inst (Ctx.E, Byt & " = mul.i64 " & Stride & ", " & Img (ESz));
                                 Emit_Inst (Ctx.E, "memcpy " & To_String (EP_S) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                              end;
                           end if;
                        end;
                        pragma Unreferenced (EP);
                     else
                        declare
                           EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, Cur_Ptr, Off64);
                        begin
                           if ETI.Kind = TK_Scalar then
                              declare
                                 V  : constant Expr_Val := Emit_Expr (Ctx, Val_Node, DTI.Elem_Type);
                                 VR : constant String :=
                                   Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (ETI.Pir_Type));
                              begin
                                 Emit_Range_Check (Ctx, VR, To_String (ETI.Pir_Type), DTI.Elem_Type);
                                 Emit_Discriminant_Check (Ctx, VR, DTI.Elem_Type, Is_Access => True);
                                 Emit_Inst (Ctx.E,
                                   "store." & To_String (ETI.Pir_Type) & " " & VR &
                                   ", " & EP2 & ", align " & Img (ETI.Align));
                              end;
                           elsif ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic then
                              -- Array-of-dynamic-array element: allocate buffer, store @__ada_arr_desc at EP2, init/copy!
                              declare
                                 IETI   : constant Type_Info := Get_Type_Info (Ctx, ETI.Elem_Type);
                                 IIs_DE : constant Boolean := IETI.Kind = TK_Constrained_Array and then IETI.Is_Dynamic;
                                 IEAl   : constant Natural := (if IIs_DE then 8 else Natural'Max (1, IETI.Align));
                                 IESz   : constant Natural := (if IIs_DE then 40 else Natural'Max (1, ((IETI.Byte_Size + IEAl - 1) / IEAl) * IEAl));
                                 IND_A  : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, 4));
                                 IFirsts, ILasts : Str_Array_4;
                                 ITot64 : Unbounded_String;
                                 IByt   : constant String := Next_Reg (Ctx.E, "aaby");
                                 INz    : constant String := Next_Reg (Ctx.E, "aanz");
                                 IAlB   : constant String := Next_Reg (Ctx.E, "aaal");
                                 IDataP : Unbounded_String;
                                 IP0    : constant String := Next_Reg (Ctx.E, "aap0");
                              begin
                                 for D_I in 1 .. IND_A loop
                                    Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, IFirsts (D_I), ILasts (D_I));
                                    declare
                                       DL : constant String := Emit_Dim_Len_I64 (Ctx, To_String (IFirsts (D_I)), To_String (ILasts (D_I)));
                                    begin
                                       if D_I = 1 then ITot64 := To_Unbounded_String (DL);
                                       else
                                          declare
                                             M : constant String := Next_Reg (Ctx.E, "aam");
                                          begin
                                             Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (ITot64) & ", " & DL);
                                             ITot64 := To_Unbounded_String (M);
                                          end;
                                       end if;
                                    end;
                                 end loop;
                                 Emit_Inst (Ctx.E, IByt & " = mul.i64 " & To_String (ITot64) & ", " & Img (IESz));
                                 Emit_Inst (Ctx.E, INz & " = cmp_gt_s.i64 " & IByt & ", 0");
                                 Emit_Inst (Ctx.E, IAlB & " = select.i64 " & INz & ", " & IByt & ", " & Img (IEAl));
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", IAlB, "ptr", IDataP);
                                 Emit_Inst (Ctx.E, IP0 & " = field_ptr @__ada_arr_desc, " & EP2 & ", 0");
                                 Emit_Inst (Ctx.E, "store.ptr " & To_String (IDataP) & ", " & IP0 & ", align 8");
                                 for D_I in 1 .. IND_A loop
                                    declare
                                       FF : constant String := Next_Reg (Ctx.E, "aaff");
                                       LF : constant String := Next_Reg (Ctx.E, "aalf");
                                    begin
                                       Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & EP2 & ", " & Img (2 * D_I - 1));
                                       Emit_Inst (Ctx.E, "store.i32 " & To_String (IFirsts (D_I)) & ", " & FF & ", align 4");
                                       Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & EP2 & ", " & Img (2 * D_I));
                                       Emit_Inst (Ctx.E, "store.i32 " & To_String (ILasts (D_I)) & ", " & LF & ", align 4");
                                    end;
                                 end loop;
                                 if not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_AGGREGATE then
                                    Emit_Init_Composite (Ctx, To_String (IDataP), DTI.Elem_Type, Val_Node, IFirsts, ILasts);
                                 else
                                    declare
                                       Is_Par : constant Boolean := not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_PARENTHESIZED;
                                       Eval_T : constant TREE :=
                                         (if Is_Par then Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, DTI.Elem_Type)))
                                          else DTI.Elem_Type);
                                       SV  : constant Expr_Val := Emit_Expr (Ctx, Val_Node, Eval_T);
                                       Tgt : Expr_Val;
                                    begin
                                       Tgt.Kind := EK_Slice; Tgt.Reg := IDataP; Tgt.Ada_Type := DTI.Elem_Type;
                                       Tgt.NDims := IND_A; Tgt.First_Regs := IFirsts; Tgt.Last_Regs := ILasts;
                                       if not Is_Par and then not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_STRING_LITERAL then
                                          Emit_Array_Length_Check (Ctx, Tgt, SV, IND_A);
                                       elsif Ctx.Enable_Checks then
                                          for D_I in 1 .. IND_A loop
                                             declare
                                                SF, SL : Unbounded_String;
                                                C1  : constant String := Next_Reg (Ctx.E, "aab1");
                                                C2  : constant String := Next_Reg (Ctx.E, "aab2");
                                                Bad : constant String := Next_Reg (Ctx.E, "aabb");
                                             begin
                                                Get_Array_Dim_Bounds (Ctx, SV, D_I, SF, SL);
                                                Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (SF) & ", " & To_String (IFirsts (D_I)));
                                                Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (SL) & ", " & To_String (ILasts (D_I)));
                                                Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                                Emit_Check_Fail_If (Ctx, Bad);
                                             end;
                                          end loop;
                                       end if;
                                       Emit_Inst (Ctx.E, "memmove " & To_String (IDataP) & ", " & To_String (SV.Reg) & ", " & IByt & ", align " & Img (IEAl));
                                    end;
                                 end if;
                              end;
                           else
                              declare
                                 function Comp_Agg_All_Named (AN : TREE; Rem_D : Natural) return Boolean is
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
                                          if Rem_D > 1 and then not Comp_Agg_All_Named (D (AS_EXP, IT), Rem_D - 1) then
                                             return False;
                                          end if;
                                       end loop;
                                       return Has_Any;
                                    end;
                                 end Comp_Agg_All_Named;
                                 Need_Exact : constant Boolean :=
                                   ETI.Kind = TK_Constrained_Array
                                   and then not Is_Void_Tree (Val_Node)
                                   and then KIND (Val_Node) not in DN_AGGREGATE | DN_STRING_LITERAL;
                              begin
                                 if ETI.Kind = TK_Constrained_Array and then ETI.NDims > 1
                                   and then not Is_Void_Tree (Val_Node) and then KIND (Val_Node) = DN_AGGREGATE
                                   and then Comp_Agg_All_Named (Val_Node, ETI.NDims)
                                 then
                                    -- LRM 4.3.2(8) (c43208b): evaluate multi-D component aggregate's choices ONCE via Pass-1!
                                    declare
                                       Base_ET : constant TREE := Subst_Base_Type (Ctx, DTI.Elem_Type);
                                       SV      : constant Expr_Val := Emit_Expr (Ctx, Val_Node, Base_ET);
                                    begin
                                       if Ctx.Enable_Checks then
                                          for D_I in 1 .. Natural'Min (ETI.NDims, 4) loop
                                             declare
                                                SF, SL, DF, DL : Unbounded_String;
                                                C1  : constant String := Next_Reg (Ctx.E, "can1");
                                                C2  : constant String := Next_Reg (Ctx.E, "can2");
                                                Bad : constant String := Next_Reg (Ctx.E, "canb");
                                             begin
                                                Get_Array_Dim_Bounds (Ctx, SV, D_I, SF, SL);
                                                Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, DF, DL);
                                                Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (SF) & ", " & To_String (DF));
                                                Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (SL) & ", " & To_String (DL));
                                                Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                                Emit_Check_Fail_If (Ctx, Bad);
                                             end;
                                          end loop;
                                       end if;
                                       Emit_Copy_Composite (Ctx, EP2, SV, DTI.Elem_Type);
                                    end;
                                 else
                                    Emit_Init_Composite (Ctx, EP2, DTI.Elem_Type, Val_Node, Exact_Bounds => Need_Exact);
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                  end Store_Elem_At;
               begin
                  Get_Dim_Lo_Hi (Dim, Lo_Reg, Hi_Reg);

                  -- First pass: handle any "others => val" along this dimension
                  -- Skip indices covered by explicit positional/named choices so `val` is evaluated ONLY for uncovered slots (LRM 4.3.2 / c43210a)
                  declare
                     type Static_Span is record
                        Lo, Hi : Integer_64 := 0;
                     end record;
                     Spans      : array (1 .. 64) of Static_Span;
                     Num_Spans  : Natural := 0;
                     Pos_Cnt    : Integer_64 := 0;
                     Has_Dyn_Ch : Boolean := False;
                     Scan       : SEQ_TYPE := Assocs;
                     SIt        : TREE;
                     Others_Exp : TREE := CONST_VOID;
                  begin
                     while not IS_EMPTY (Scan) loop
                        POP (Scan, SIt);
                        if KIND (SIt) = DN_ASSOC or else KIND (SIt) = DN_NAMED then
                           declare
                              CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, SIt));
                              Ch : TREE;
                           begin
                              while not IS_EMPTY (CS) loop
                                 POP (CS, Ch);
                                 if KIND (Ch) = DN_CHOICE_OTHERS then
                                    Others_Exp := D (AS_EXP, SIt);
                                 elsif KIND (Ch) = DN_CHOICE_EXP then
                                    declare
                                       SV : constant TREE := Safe_SM_Value (D (AS_EXP, Ch));
                                    begin
                                       if Has_Static_Val (SV) and then Num_Spans < 64 then
                                          Num_Spans := Num_Spans + 1;
                                          Spans (Num_Spans) := (Lo => Eval_Int64 (SV), Hi => Eval_Int64 (SV));
                                       else
                                          Has_Dyn_Ch := True;
                                       end if;
                                    end;
                                 elsif KIND (Ch) = DN_CHOICE_RANGE then
                                    declare
                                       DR : constant TREE := D (AS_DISCRETE_RANGE, Ch);
                                    begin
                                       if not Is_Void_Tree (DR) and then KIND (DR) = DN_RANGE
                                         and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, DR)))
                                         and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, DR)))
                                         and then Num_Spans < 64
                                       then
                                          Num_Spans := Num_Spans + 1;
                                          Spans (Num_Spans) := (Lo => Eval_Int64 (Safe_SM_Value (D (AS_EXP1, DR))),
                                                                Hi => Eval_Int64 (Safe_SM_Value (D (AS_EXP2, DR))));
                                       else
                                          Has_Dyn_Ch := True;
                                       end if;
                                    end;
                                 end if;
                              end loop;
                           end;
                        else
                           Pos_Cnt := Pos_Cnt + 1;
                        end if;
                     end loop;

                     if not Is_Void_Tree (Others_Exp) then
                        declare
                           Tot64  : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Lo_Reg), To_String (Hi_Reg));
                           L_Hdr  : constant String := Next_Label (Ctx.E, "agg_hdr");
                           L_Body : constant String := Next_Label (Ctx.E, "agg_bdy");
                           L_Step : constant String := Next_Label (Ctx.E, "agg_stp");
                           L_Done : constant String := Next_Label (Ctx.E, "agg_end");
                           I_Reg  : constant String := Next_Reg (Ctx.E, "ai");
                           Cond   : constant String := Next_Reg (Ctx.E, "ac");
                           Next_I : constant String := Next_Reg (Ctx.E, "ain");
                           Start64 : constant String :=
                             (if Pos_Cnt > 0 and then Num_Spans = 0 and then not Has_Dyn_Ch
                              then Img (Long_Long_Integer (Pos_Cnt)) else "0");
                        begin
                           Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Start64 & ")");
                           Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                           Emit_Inst (Ctx.E, Cond & " = cmp_lt_s.i64 " & I_Reg & ", " & Tot64);
                           Emit_Term (Ctx.E, "br " & Cond & ", " & L_Body & ", " & L_Done);
                           Emit_Label (Ctx.E, L_Body);
                           if Num_Spans > 0 and then not Has_Dyn_Ch then
                              -- Compute actual Ada index Idx64 = sext(Lo_Reg) + I_Reg and skip if covered by any static span
                              declare
                                 Lo64  : constant String := Next_Reg (Ctx.E, "olo6");
                                 Idx64 : constant String := Next_Reg (Ctx.E, "oidx");
                                 Cov   : Unbounded_String;
                                 L_Do  : constant String := Next_Label (Ctx.E, "agg_do");
                              begin
                                 Emit_Inst (Ctx.E, Lo64 & " = sext.i64 " & To_String (Lo_Reg));
                                 Emit_Inst (Ctx.E, Idx64 & " = add.i64 " & Lo64 & ", " & I_Reg);
                                 for S_I in 1 .. Num_Spans loop
                                    declare
                                       C1 : constant String := Next_Reg (Ctx.E, "oc1");
                                       C2 : constant String := Next_Reg (Ctx.E, "oc2");
                                       InS : constant String := Next_Reg (Ctx.E, "oin");
                                    begin
                                       Emit_Inst (Ctx.E, C1 & " = cmp_ge_s.i64 " & Idx64 & ", " & Img (Long_Long_Integer (Spans (S_I).Lo)));
                                       Emit_Inst (Ctx.E, C2 & " = cmp_le_s.i64 " & Idx64 & ", " & Img (Long_Long_Integer (Spans (S_I).Hi)));
                                       Emit_Inst (Ctx.E, InS & " = and.i8 " & C1 & ", " & C2);
                                       if S_I = 1 then
                                          Cov := To_Unbounded_String (InS);
                                       else
                                          declare
                                             OrR : constant String := Next_Reg (Ctx.E, "oor");
                                          begin
                                             Emit_Inst (Ctx.E, OrR & " = or.i8 " & To_String (Cov) & ", " & InS);
                                             Cov := To_Unbounded_String (OrR);
                                          end;
                                       end if;
                                    end;
                                 end loop;
                                 Emit_Term (Ctx.E, "br " & To_String (Cov) & ", " & L_Step & ", " & L_Do);
                                 Emit_Label (Ctx.E, L_Do);
                              end;
                           end if;
                           Store_Elem_At (I_Reg, Others_Exp);
                           Emit_Term (Ctx.E, "jmp " & L_Step);
                           Emit_Label (Ctx.E, L_Step);
                           Emit_Inst (Ctx.E, Next_I & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                           Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Next_I & ")");
                           Emit_Label (Ctx.E, L_Done);
                        end;
                     end if;
                     -- Check positional element count against dimension length when no OTHERS (c48009e, c48009f)
                     if Ctx.Enable_Checks and then Is_Void_Tree (Others_Exp)
                       and then Pos_Cnt > 0 and then Num_Spans = 0 and then not Has_Dyn_Ch
                     then
                        declare
                           DLen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Lo_Reg), To_String (Hi_Reg));
                           Mis  : constant String := Next_Reg (Ctx.E, "apmis");
                        begin
                           Emit_Inst (Ctx.E, Mis & " = cmp_ne.i64 " & DLen & ", " & Img (Long_Long_Integer (Pos_Cnt)));
                           Emit_Check_Fail_If (Ctx, Mis);
                        end;
                        -- LRM 4.3.2(9) (c43212a Case 1A): in unconstrained multi-D aggregate, a positional subaggregate
                        -- has lower bound Base_Idx'FIRST, which must match Lo_Reg from any named sibling subaggregate!
                        if DTI.Kind = TK_Unconstrained_Array and then Dim > 1
                          and then Dim <= DTI.NDims and then not Is_Void_Tree (DTI.Dims (Dim).Idx_Type)
                        then
                           declare
                              BF, BL : Unbounded_String;
                              BMis   : constant String := Next_Reg (Ctx.E, "aplo");
                           begin
                              Emit_Index_Bounds (Ctx, DTI.Dims (Dim).Idx_Type, BF, BL);
                              Emit_Inst (Ctx.E, BMis & " = cmp_ne.i32 " & To_String (Lo_Reg) & ", " & To_String (BF));
                              Emit_Check_Fail_If (Ctx, BMis);
                           end;
                        end if;
                     end if;
                  end;

                  -- Second pass: positional and named explicit elements
                  declare
                     Has_Named_R : Boolean := False;
                     Has_Others2 : Boolean := False;
                     Min_C32, Max_C32 : Unbounded_String;
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
                              if KIND (Ch) = DN_CHOICE_OTHERS then
                                 Has_Others2 := True;
                              elsif KIND (Ch) = DN_CHOICE_EXP then
                                 declare
                                    CE  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch));
                                    C32 : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i32");
                                    Off : constant String := Next_Reg (Ctx.E, "aoff");
                                    O64 : constant String := Next_Reg (Ctx.E, "aoff64");
                                 begin
                                    if not Has_Named_R then
                                       Min_C32 := To_Unbounded_String (C32);
                                       Max_C32 := To_Unbounded_String (C32);
                                       Has_Named_R := True;
                                    else
                                       declare
                                          CL : constant String := Next_Reg (Ctx.E, "acl");
                                          ML : constant String := Next_Reg (Ctx.E, "aml");
                                          CH : constant String := Next_Reg (Ctx.E, "ach");
                                          MH : constant String := Next_Reg (Ctx.E, "amh");
                                       begin
                                          Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i32 " & C32 & ", " & To_String (Min_C32));
                                          Emit_Inst (Ctx.E, ML & " = select.i32 " & CL & ", " & C32 & ", " & To_String (Min_C32));
                                          Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i32 " & C32 & ", " & To_String (Max_C32));
                                          Emit_Inst (Ctx.E, MH & " = select.i32 " & CH & ", " & C32 & ", " & To_String (Max_C32));
                                          Min_C32 := To_Unbounded_String (ML);
                                          Max_C32 := To_Unbounded_String (MH);
                                       end;
                                    end if;
                                    if Ctx.Enable_Checks then
                                       -- Check choice lies in BASE array index subtype (c43211a) AND within destination buffer [Lo_Reg .. Hi_Reg]
                                       declare
                                          BTI_C : constant Type_Info := Get_Type_Info (Ctx, Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type))));
                                       begin
                                          if Dim <= BTI_C.NDims and then not Is_Void_Tree (BTI_C.Dims (Dim).Idx_Type) then
                                             declare
                                                IT  : constant TREE := BTI_C.Dims (Dim).Idx_Type;
                                                ITI : constant Type_Info := Get_Type_Info (Ctx, IT);
                                                CC  : constant String := Emit_Int_Convert (Ctx, C32, "i32", To_String (ITI.Pir_Type));
                                             begin
                                                Emit_Range_Check (Ctx, CC, To_String (ITI.Pir_Type), IT);
                                             end;
                                          end if;
                                       end;
                                       declare
                                          B1  : constant String := Next_Reg (Ctx.E, "acb1");
                                          B2  : constant String := Next_Reg (Ctx.E, "acb2");
                                          Bad : constant String := Next_Reg (Ctx.E, "acbd");
                                       begin
                                          Emit_Inst (Ctx.E, B1 & " = cmp_lt_s.i32 " & C32 & ", " & To_String (Lo_Reg));
                                          Emit_Inst (Ctx.E, B2 & " = cmp_gt_s.i32 " & C32 & ", " & To_String (Hi_Reg));
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & B1 & ", " & B2);
                                          Emit_Check_Fail_If (Ctx, Bad);
                                       end;
                                    end if;
                                    Emit_Inst (Ctx.E, Off & " = sub.i32 " & C32 & ", " & To_String (Lo_Reg));
                                    Emit_Inst (Ctx.E, O64 & " = sext.i64 " & Off);
                                    Store_Elem_At (O64, D (AS_EXP, Item));
                                 end;
                              elsif KIND (Ch) = DN_CHOICE_RANGE then
                                 declare
                                    RT, RL, RH : Unbounded_String;
                                    L_H : constant String := Next_Label (Ctx.E, "ar_h");
                                    L_B : constant String := Next_Label (Ctx.E, "ar_b");
                                    L_E : constant String := Next_Label (Ctx.E, "ar_e");
                                    IV  : constant String := Next_Reg (Ctx.E, "ari");
                                    Cnd : constant String := Next_Reg (Ctx.E, "arc");
                                    L32, H32 : Unbounded_String;
                                 begin
                                    if Ctx.Saved_Type_Ranges.Contains (Tree_Key (Ch)) then
                                       declare
                                          SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (Ch));
                                       begin
                                          L32 := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), "i32", 4, SR.Is_Global));
                                          H32 := To_Unbounded_String (Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), "i32", 4, SR.Is_Global));
                                       end;
                                    else
                                       Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                       if Ctx.Enable_Checks and then To_String (RT) = "i64" then
                                          declare
                                             BTI_64 : constant Type_Info := Get_Type_Info (Ctx, Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type))));
                                          begin
                                             if Dim <= BTI_64.NDims and then not Is_Void_Tree (BTI_64.Dims (Dim).Idx_Type) then
                                                declare
                                                   NE64  : constant String := Next_Reg (Ctx.E, "ar64ne");
                                                   L_C64 : constant String := Next_Label (Ctx.E, "ar64c");
                                                   L_O64 : constant String := Next_Label (Ctx.E, "ar64o");
                                                begin
                                                   Emit_Inst (Ctx.E, NE64 & " = cmp_le_s.i64 " & To_String (RL) & ", " & To_String (RH));
                                                   Emit_Term (Ctx.E, "br " & NE64 & ", " & L_C64 & ", " & L_O64);
                                                   Emit_Label (Ctx.E, L_C64);
                                                   Emit_Range_Check (Ctx, To_String (RL), "i64", BTI_64.Dims (Dim).Idx_Type);
                                                   Emit_Range_Check (Ctx, To_String (RH), "i64", BTI_64.Dims (Dim).Idx_Type);
                                                   Emit_Term (Ctx.E, "jmp " & L_O64);
                                                   Emit_Label (Ctx.E, L_O64);
                                                end;
                                             end if;
                                          end;
                                       end if;
                                       L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                                       H32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                                    end if;
                                    if not Has_Named_R then
                                       Min_C32 := L32;
                                       Max_C32 := H32;
                                       Has_Named_R := True;
                                    else
                                       declare
                                          CL : constant String := Next_Reg (Ctx.E, "acl");
                                          ML : constant String := Next_Reg (Ctx.E, "aml");
                                          CH : constant String := Next_Reg (Ctx.E, "ach");
                                          MH : constant String := Next_Reg (Ctx.E, "amh");
                                       begin
                                          Emit_Inst (Ctx.E, CL & " = cmp_lt_s.i32 " & To_String (L32) & ", " & To_String (Min_C32));
                                          Emit_Inst (Ctx.E, ML & " = select.i32 " & CL & ", " & To_String (L32) & ", " & To_String (Min_C32));
                                          Emit_Inst (Ctx.E, CH & " = cmp_gt_s.i32 " & To_String (H32) & ", " & To_String (Max_C32));
                                          Emit_Inst (Ctx.E, MH & " = select.i32 " & CH & ", " & To_String (H32) & ", " & To_String (Max_C32));
                                          Min_C32 := To_Unbounded_String (ML);
                                          Max_C32 := To_Unbounded_String (MH);
                                       end;
                                    end if;
                                    if Ctx.Enable_Checks then
                                       -- Non-null choice range must lie in BASE index subtype (c43211a) and within [Lo_Reg .. Hi_Reg] (c43206a)
                                       declare
                                          NE    : constant String := Next_Reg (Ctx.E, "arne");
                                          L_Chk : constant String := Next_Label (Ctx.E, "arc");
                                          L_Ok  : constant String := Next_Label (Ctx.E, "aro");
                                          B1    : constant String := Next_Reg (Ctx.E, "arb1");
                                          B2    : constant String := Next_Reg (Ctx.E, "arb2");
                                          Bad   : constant String := Next_Reg (Ctx.E, "arbd");
                                          BTI_C : constant Type_Info := Get_Type_Info (Ctx, Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Dst_Type))));
                                       begin
                                          Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (L32) & ", " & To_String (H32));
                                          Emit_Term (Ctx.E, "br " & NE & ", " & L_Chk & ", " & L_Ok);
                                          Emit_Label (Ctx.E, L_Chk);
                                          if Dim <= BTI_C.NDims and then not Is_Void_Tree (BTI_C.Dims (Dim).Idx_Type)
                                            and then Scalar_Bits (BTI_C.Dims (Dim).Idx_Type) <= 32
                                          then
                                             declare
                                                IT  : constant TREE := BTI_C.Dims (Dim).Idx_Type;
                                                ITI : constant Type_Info := Get_Type_Info (Ctx, IT);
                                                LC  : constant String := Emit_Int_Convert (Ctx, To_String (L32), "i32", To_String (ITI.Pir_Type));
                                                HC  : constant String := Emit_Int_Convert (Ctx, To_String (H32), "i32", To_String (ITI.Pir_Type));
                                             begin
                                                Emit_Range_Check (Ctx, LC, To_String (ITI.Pir_Type), IT);
                                                Emit_Range_Check (Ctx, HC, To_String (ITI.Pir_Type), IT);
                                             end;
                                          end if;
                                          Emit_Inst (Ctx.E, B1 & " = cmp_lt_s.i32 " & To_String (L32) & ", " & To_String (Lo_Reg));
                                          Emit_Inst (Ctx.E, B2 & " = cmp_gt_s.i32 " & To_String (H32) & ", " & To_String (Hi_Reg));
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & B1 & ", " & B2);
                                          Emit_Check_Fail_If (Ctx, Bad);
                                          Emit_Term (Ctx.E, "jmp " & L_Ok);
                                          Emit_Label (Ctx.E, L_Ok);
                                       end;
                                    end if;
                                    Emit_Term (Ctx.E, "jmp " & L_H & "(" & To_String (L32) & ")");
                                    Emit_Label (Ctx.E, L_H, IV & ": i32");
                                    Emit_Inst (Ctx.E, Cnd & " = cmp_le_s.i32 " & IV & ", " & To_String (H32));
                                    Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                                    Emit_Label (Ctx.E, L_B);
                                    declare
                                       Off : constant String := Next_Reg (Ctx.E, "aro");
                                       O64 : constant String := Next_Reg (Ctx.E, "aro64");
                                       Nx  : constant String := Next_Reg (Ctx.E, "arn");
                                    begin
                                       Emit_Inst (Ctx.E, Off & " = sub.i32 " & IV & ", " & To_String (Lo_Reg));
                                       Emit_Inst (Ctx.E, O64 & " = sext.i64 " & Off);
                                       Store_Elem_At (O64, D (AS_EXP, Item));
                                       Emit_Inst (Ctx.E, Nx & " = add.i32 " & IV & ", 1");
                                       Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                                    end;
                                    Emit_Label (Ctx.E, L_E);
                                 end;
                              end if;
                           end loop;
                        end;
                     else
                        -- Positional element
                        Store_Elem_At (Img (Long_Long_Integer (Pos)), Item);
                        Pos := Pos + 1;
                     end if;
                  end loop;
                  -- In qualified/exact-bounds or multidimensional subaggregate (Dim > 1), named bounds MUST match [Lo_Reg .. Hi_Reg] (c43212a/c, c48009e/f);
                  -- otherwise named aggregate without OTHERS must at least match dimension length (c48009f CASE 2)!
                  if Ctx.Enable_Checks and then Has_Named_R and then not Has_Others2 then
                     if Exact_Bounds or else Dim > 1 then
                        declare
                           E1  : constant String := Next_Reg (Ctx.E, "aeb1");
                           E2  : constant String := Next_Reg (Ctx.E, "aeb2");
                           Bad : constant String := Next_Reg (Ctx.E, "aebb");
                        begin
                           Emit_Inst (Ctx.E, E1 & " = cmp_ne.i32 " & To_String (Min_C32) & ", " & To_String (Lo_Reg));
                           Emit_Inst (Ctx.E, E2 & " = cmp_ne.i32 " & To_String (Max_C32) & ", " & To_String (Hi_Reg));
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & E1 & ", " & E2);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end;
                     else
                        declare
                           ALen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Min_C32), To_String (Max_C32));
                           DLen : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Lo_Reg), To_String (Hi_Reg));
                           Mis  : constant String := Next_Reg (Ctx.E, "anmis");
                        begin
                           Emit_Inst (Ctx.E, Mis & " = cmp_ne.i64 " & ALen & ", " & DLen);
                           Emit_Check_Fail_If (Ctx, Mis);
                        end;
                     end if;
                  end if;
                  end;
               end Emit_Init_Array_Dim;
            begin
               Emit_Init_Array_Dim (Dst_Ptr, Init_Node, 1);
            end;
            return;
         else
            return;
         end if;
      end if;

      -- General composite expression copy
      declare
         Src : constant Expr_Val := Emit_Expr (Ctx, Init_Node, Dst_Type);
      begin
         if Exact_Bounds and then Ctx.Enable_Checks and then DTI.Kind = TK_Constrained_Array then
            declare
               ND_A : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, 4));
            begin
               for D_I in 1 .. ND_A loop
                  declare
                     SF, SL, DF, DL : Unbounded_String;
                     C1  : constant String := Next_Reg (Ctx.E, "exb1");
                     C2  : constant String := Next_Reg (Ctx.E, "exb2");
                     Bad : constant String := Next_Reg (Ctx.E, "exbb");
                  begin
                     Get_Array_Dim_Bounds (Ctx, Src, D_I, SF, SL);
                     if Length (Dst_Firsts (D_I)) > 0 then
                        DF := Dst_Firsts (D_I);
                        DL := Dst_Lasts (D_I);
                     else
                        Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, DF, DL);
                     end if;
                     Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (SF) & ", " & To_String (DF));
                     Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (SL) & ", " & To_String (DL));
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end loop;
            end;
         end if;
         Emit_Copy_Composite (Ctx, Dst_Ptr, Src, Dst_Type);
      end;
   end Emit_Init_Composite;

   function Is_Bitfield (FE : Field_Entry) return Boolean is
   begin
      return FE.Has_Explicit_Offset
        and then FE.Bit_Size > 0
        and then (FE.Bit_Offset > 0 or else FE.Bit_Size < FE.Field_Size * 8);
   end Is_Bitfield;

   function Emit_Bitfield_Load
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      FE        : Field_Entry;
      Comp_Type : TREE) return String
   is
      TI      : constant Type_Info := Get_Type_Info (Ctx, Comp_Type);
      N_Bytes : constant Natural := Natural'Min (8, (FE.Bit_Offset + FE.Bit_Size + 7) / 8);
      Acc     : Unbounded_String;
      Sh_R    : Unbounded_String;
      Masked  : Unbounded_String;
      Is_Sig  : Boolean := False;
   begin
      if Is_Integer_Type (Comp_Type) then
         declare
            St     : Boolean := False;
            Lo, Hi : Integer_64 := 0;
         begin
            Get_Discrete_Bounds (Comp_Type, St, Lo, Hi);
            Is_Sig := (not St) or else Lo < 0;
         end;
      elsif Is_Fixed_Type (Comp_Type) then
         Is_Sig := True;
      end if;
      for K in 0 .. N_Bytes - 1 loop
         declare
            Off_R : constant String := Next_Reg (Ctx.E, "bfo");
            BP    : constant String := Next_Reg (Ctx.E, "bfp");
            BV    : constant String := Next_Reg (Ctx.E, "bfv");
            B64   : constant String := Next_Reg (Ctx.E, "bf6");
            ShV   : Unbounded_String := To_Unbounded_String (B64);
         begin
            Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset + K));
            Emit_Inst (Ctx.E, BP & " = ptr_add.i64 " & Rec_Ptr & ", " & Off_R & " [inbounds]");
            Emit_Inst (Ctx.E, BV & " = load.i8 " & BP & ", align 1");
            Emit_Inst (Ctx.E, B64 & " = zext.i64 " & BV);
            if K > 0 then
               declare
                  SR : constant String := Next_Reg (Ctx.E, "bfs");
               begin
                  Emit_Inst (Ctx.E, SR & " = shl.i64 " & B64 & ", " & Img (K * 8));
                  ShV := To_Unbounded_String (SR);
               end;
            end if;
            if K = 0 then
               Acc := ShV;
            else
               declare
                  OR_R : constant String := Next_Reg (Ctx.E, "bfr");
               begin
                  Emit_Inst (Ctx.E, OR_R & " = or.i64 " & To_String (Acc) & ", " & To_String (ShV));
                  Acc := To_Unbounded_String (OR_R);
               end;
            end if;
         end;
      end loop;
      if FE.Bit_Offset > 0 then
         declare
            SR : constant String := Next_Reg (Ctx.E, "bfrsh");
         begin
            Emit_Inst (Ctx.E, SR & " = shr.i64 " & To_String (Acc) & ", " & Img (FE.Bit_Offset));
            Sh_R := To_Unbounded_String (SR);
         end;
      else
         Sh_R := Acc;
      end if;
      if FE.Bit_Size < 64 then
         if Is_Sig then
            declare
               L_Sh : constant String := Next_Reg (Ctx.E, "bfsl");
               R_Sh : constant String := Next_Reg (Ctx.E, "bfsr");
               Rem_B : constant Natural := 64 - FE.Bit_Size;
            begin
               Emit_Inst (Ctx.E, L_Sh & " = shl.i64 " & To_String (Sh_R) & ", " & Img (Rem_B));
               Emit_Inst (Ctx.E, R_Sh & " = sar.i64 " & L_Sh & ", " & Img (Rem_B));
               Masked := To_Unbounded_String (R_Sh);
            end;
         else
            declare
               M_Val : constant Unsigned_64 := Shift_Left (Unsigned_64 (1), FE.Bit_Size) - 1;
               MR    : constant String := Next_Reg (Ctx.E, "bfm");
               AR    : constant String := Next_Reg (Ctx.E, "bfa");
            begin
               Emit_Inst (Ctx.E, MR & " = const.i64 " & Img (Long_Long_Integer (M_Val)));
               Emit_Inst (Ctx.E, AR & " = and.i64 " & To_String (Sh_R) & ", " & MR);
               Masked := To_Unbounded_String (AR);
            end;
         end if;
      else
         Masked := Sh_R;
      end if;
      if To_String (TI.Pir_Type) = "f32" then
         declare
            I32 : constant String := Emit_Int_Convert (Ctx, To_String (Masked), "i64", "i32");
            F32 : constant String := Next_Reg (Ctx.E, "bff32");
         begin
            Emit_Inst (Ctx.E, F32 & " = bitcast.f32 " & I32);
            return F32;
         end;
      elsif To_String (TI.Pir_Type) = "f64" then
         declare
            F64 : constant String := Next_Reg (Ctx.E, "bff64");
         begin
            Emit_Inst (Ctx.E, F64 & " = bitcast.f64 " & To_String (Masked));
            return F64;
         end;
      elsif To_String (TI.Pir_Type) = "ptr" then
         declare
            PR : constant String := Next_Reg (Ctx.E, "bfptr");
         begin
            Emit_Inst (Ctx.E, PR & " = inttoptr.ptr " & To_String (Masked));
            return PR;
         end;
      else
         return Emit_Int_Convert (Ctx, To_String (Masked), "i64", To_String (TI.Pir_Type));
      end if;
   end Emit_Bitfield_Load;

   procedure Emit_Bitfield_Store
     (Ctx          : in out Context;
      Rec_Ptr      : String;
      FE           : Field_Entry;
      Val_Reg      : String;
      Val_Pir_Type : String;
      Comp_Type    : TREE)
   is
      pragma Unreferenced (Comp_Type);
      N_Bytes : constant Natural := Natural'Min (8, (FE.Bit_Offset + FE.Bit_Size + 7) / 8);
      V64     : Unbounded_String;
      Shifted : Unbounded_String;
   begin
      if Val_Pir_Type = "f32" then
         declare
            I32 : constant String := Next_Reg (Ctx.E, "bsi32");
            I64 : constant String := Next_Reg (Ctx.E, "bsi64");
         begin
            Emit_Inst (Ctx.E, I32 & " = bitcast.i32 " & Val_Reg);
            Emit_Inst (Ctx.E, I64 & " = zext.i64 " & I32);
            V64 := To_Unbounded_String (I64);
         end;
      elsif Val_Pir_Type = "f64" then
         declare
            I64 : constant String := Next_Reg (Ctx.E, "bsi64");
         begin
            Emit_Inst (Ctx.E, I64 & " = bitcast.i64 " & Val_Reg);
            V64 := To_Unbounded_String (I64);
         end;
      elsif Val_Pir_Type = "ptr" then
         declare
            I64 : constant String := Next_Reg (Ctx.E, "bsp64");
         begin
            Emit_Inst (Ctx.E, I64 & " = ptrtoint.i64 " & Val_Reg);
            V64 := To_Unbounded_String (I64);
         end;
      else
         V64 := To_Unbounded_String (Emit_Int_Convert (Ctx, Val_Reg, Val_Pir_Type, "i64"));
      end if;
      if FE.Bit_Size < 64 then
         declare
            M_Val : constant Unsigned_64 := Shift_Left (Unsigned_64 (1), FE.Bit_Size) - 1;
            MR    : constant String := Next_Reg (Ctx.E, "bsm");
            AR    : constant String := Next_Reg (Ctx.E, "bsa");
         begin
            Emit_Inst (Ctx.E, MR & " = const.i64 " & Img (Long_Long_Integer (M_Val)));
            Emit_Inst (Ctx.E, AR & " = and.i64 " & To_String (V64) & ", " & MR);
            V64 := To_Unbounded_String (AR);
         end;
      end if;
      if FE.Bit_Offset > 0 then
         declare
            SR : constant String := Next_Reg (Ctx.E, "bssh");
         begin
            Emit_Inst (Ctx.E, SR & " = shl.i64 " & To_String (V64) & ", " & Img (FE.Bit_Offset));
            Shifted := To_Unbounded_String (SR);
         end;
      else
         Shifted := V64;
      end if;
      for K in 0 .. N_Bytes - 1 loop
         declare
            Lo_In_Byte : constant Natural := (if K = 0 then FE.Bit_Offset else 0);
            End_Tot    : constant Natural := FE.Bit_Offset + FE.Bit_Size;
            Hi_In_Byte : constant Natural := Natural'Min (8, End_Tot - K * 8);
            Bits_Here  : constant Natural := Hi_In_Byte - Lo_In_Byte;
            Byte_Mask  : constant Natural := Natural (Shift_Left (Shift_Left (Unsigned_64 (1), Bits_Here) - 1, Lo_In_Byte));
            Off_R      : constant String := Next_Reg (Ctx.E, "bso");
            BP         : constant String := Next_Reg (Ctx.E, "bsp");
            Byte_Val   : Unbounded_String;
         begin
            Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset + K));
            Emit_Inst (Ctx.E, BP & " = ptr_add.i64 " & Rec_Ptr & ", " & Off_R & " [inbounds]");
            if K > 0 then
               declare
                  RSh : constant String := Next_Reg (Ctx.E, "bsr");
                  Tr  : constant String := Next_Reg (Ctx.E, "bst");
               begin
                  Emit_Inst (Ctx.E, RSh & " = shr.i64 " & To_String (Shifted) & ", " & Img (K * 8));
                  Emit_Inst (Ctx.E, Tr & " = trunc.i8 " & RSh);
                  Byte_Val := To_Unbounded_String (Tr);
               end;
            else
               declare
                  Tr : constant String := Next_Reg (Ctx.E, "bst");
               begin
                  Emit_Inst (Ctx.E, Tr & " = trunc.i8 " & To_String (Shifted));
                  Byte_Val := To_Unbounded_String (Tr);
               end;
            end if;
            if Byte_Mask = 255 then
               Emit_Inst (Ctx.E, "store.i8 " & To_String (Byte_Val) & ", " & BP & ", align 1");
            else
               declare
                  Old_B : constant String := Next_Reg (Ctx.E, "bsol");
                  Clr_M : constant Natural := 255 - Byte_Mask;
                  Cl_B  : constant String := Next_Reg (Ctx.E, "bscl");
                  New_B : constant String := Next_Reg (Ctx.E, "bsnw");
                  An_B  : constant String := Next_Reg (Ctx.E, "bsan");
               begin
                  Emit_Inst (Ctx.E, Old_B & " = load.i8 " & BP & ", align 1");
                  Emit_Inst (Ctx.E, Cl_B & " = and.i8 " & Old_B & ", " & Img (Clr_M));
                  Emit_Inst (Ctx.E, An_B & " = and.i8 " & To_String (Byte_Val) & ", " & Img (Byte_Mask));
                  Emit_Inst (Ctx.E, New_B & " = or.i8 " & Cl_B & ", " & An_B);
                  Emit_Inst (Ctx.E, "store.i8 " & New_B & ", " & BP & ", align 1");
               end;
            end if;
         end;
      end loop;
   end Emit_Bitfield_Store;

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
      if Activate_Now then
         if Ctx.Enable_Checks and then Length (TI.Elab_Flag) > 0 then
            declare
               FA : constant String := Next_Reg (Ctx.E, "tfa");
               FV : constant String := Next_Reg (Ctx.E, "tfv");
               NE : constant String := Next_Reg (Ctx.E, "tne");
               L_Bad : constant String := Next_Label (Ctx.E, "tpe_b");
               L_Ok  : constant String := Next_Label (Ctx.E, "tpe_o");
            begin
               Emit_Inst (Ctx.E, FA & " = addr " & To_String (TI.Elab_Flag));
               Emit_Inst (Ctx.E, FV & " = load.i8 " & FA & ", align 1");
               Emit_Inst (Ctx.E, NE & " = cmp_eq.i8 " & FV & ", 0");
               Emit_Term (Ctx.E, "br " & NE & ", " & L_Bad & ", " & L_Ok);
               Emit_Label (Ctx.E, L_Bad);
               Emit_Raise_Tag (Ctx, Program_Error_Tag (Ctx));
               Emit_Label (Ctx.E, L_Ok);
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
                    To_String (M_Reg) & ", " & Fn_Ptr & ", " & To_String (Env_R) & ", " & Img (TI.Num_Entries),
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
                 To_String (M_Reg) & ", " & Fn_Ptr & ", " & To_String (Env_R) & ", " & Img (TI.Num_Entries),
                 "ptr", TH_Reg);
            end if;
         end;
      else
         Emit_Call_Or_Invoke (Ctx, "@__ada_task_create",
           To_String (M_Reg) & ", " & Fn_Ptr & ", " & To_String (Env_R) & ", " & Img (TI.Num_Entries),
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
               FA    : constant String := Next_Reg (Ctx.E, "tfa");
               Dummy : Unbounded_String;
            begin
               Emit_Inst (Ctx.E, FA & " = addr " & To_String (TI.Elab_Flag));
               Emit_Call_Or_Invoke (Ctx, "@__ada_task_set_elab_flag",
                 To_String (TH_Reg) & ", " & FA, "", Dummy, Nounwind => True);
            end;
         end if;
      end if;
      return To_String (TH_Reg);
   end Emit_Create_Task_Handle;

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

   procedure Emit_Default_Init
     (Ctx            : in out Context;
      Dst_Ptr        : String;
      Dst_Type       : TREE;
      Activate_Tasks : Boolean := False)
   is
      RT  : constant TREE := Subst_Resolve_Type (Ctx, Dst_Type);
      BT  : constant TREE := Subst_Base_Type (Ctx, RT);
      DTI : constant Type_Info := Get_Type_Info (Ctx, RT);
   begin
      if Is_Void_Tree (RT) then
         return;
      end if;
      if Is_Task_Type (RT) then
         declare
            TH : constant String := Emit_Create_Task_Handle (Ctx, RT, Activate_Now => Activate_Tasks);
         begin
            Emit_Inst (Ctx.E, "store.ptr " & TH & ", " & Dst_Ptr & ", align 8");
         end;
         return;
      elsif Is_Access_Type (RT) then
         Emit_Inst (Ctx.E, "store.ptr 0, " & Dst_Ptr & ", align 8");
         return;
      elsif DTI.Kind = TK_Record and then not Is_Void_Tree (BT) and then KIND (BT) = DN_RECORD then
         -- Zero record storage so unused variant-union padding bytes are deterministic (e.g. c37215f)
         if DTI.Byte_Size > 0 then
            Emit_Inst (Ctx.E, "memset " & Dst_Ptr & ", 0, " & Img (DTI.Byte_Size) & ", align " & Img (Natural'Max (1, DTI.Align)));
         end if;
         -- 1. Discriminants (constrained values take precedence over default values)
         declare
            FIdx : Natural := 0;
            DS   : constant TREE := D (SM_DISCRIMINANT_S, BT);
            ND   : constant TREE := (if KIND (RT) = DN_CONSTRAINED_RECORD then D (SM_NORMALIZED_DSCRMT_S, RT) else CONST_VOID);
            NDL  : SEQ_TYPE := (if Is_Void_Tree (ND) then MAKE else LIST (ND));
            Bound_Discs : U32_Vecs.Vector;
         begin
            if not Is_Void_Tree (DS) then
               declare
                  DL : SEQ_TYPE := LIST (DS);
                  DD : TREE;
               begin
                  while not IS_EMPTY (DL) loop
                     POP (DL, DD);
                     if KIND (DD) = DN_DSCRMT_DECL then
                        declare
                           NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                           DID : TREE;
                           Def_E : constant TREE := D (AS_EXP, DD);
                        begin
                           while not IS_EMPTY (NS) loop
                              POP (NS, DID);
                              declare
                                 CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                 CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                 Init_E : TREE := D (SM_INIT_EXP, DID);
                                 FP  : constant String := Next_Reg (Ctx.E, "dfp");
                                 DK  : constant Unsigned_32 := Tree_Key (Canonical_Def (DID));
                                 DVInfo : Var_Info;
                              begin
                                 if Is_Void_Tree (Init_E) then
                                    Init_E := Def_E;
                                 end if;
                                 if not IS_EMPTY (NDL) then
                                    POP (NDL, Init_E);
                                 end if;
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                   Dst_Ptr & ", " & Img (FIdx));
                                 if not Is_Void_Tree (Init_E) then
                                    declare
                                       VR : Unbounded_String;
                                    begin
                                       if KIND (RT) = DN_CONSTRAINED_RECORD
                                         and then Ctx.Saved_Type_Discs.Contains (Tree_Key (RT))
                                         and then (Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Is_Global
                                                   or else Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Owner_Subp = Ctx.Cur_Subp_Key)
                                         and then FIdx + 1 <= Ctx.Saved_Type_Discs.Element (Tree_Key (RT)).Count
                                       then
                                          declare
                                             SD : constant Saved_Disc_Info := Ctx.Saved_Type_Discs.Element (Tree_Key (RT));
                                             LV : constant String := Load_Saved_Slot (Ctx, To_String (SD.Ptrs (FIdx + 1)), To_String (SD.Pir_Types (FIdx + 1)), SD.Aligns (FIdx + 1), SD.Is_Global);
                                          begin
                                             VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, LV, To_String (SD.Pir_Types (FIdx + 1)), To_String (CTI.Pir_Type)));
                                          end;
                                       else
                                          declare
                                             DV : constant Expr_Val := Emit_Expr (Ctx, Init_E, CT);
                                          begin
                                             VR := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (DV.Reg), To_String (DV.Pir_Type), To_String (CTI.Pir_Type)));
                                             Emit_Range_Check (Ctx, To_String (VR), To_String (CTI.Pir_Type), CT);
                                          end;
                                       end if;
                                       Emit_Inst (Ctx.E,
                                         "store." & To_String (CTI.Pir_Type) & " " & To_String (VR) &
                                         ", " & FP & ", align " & Img (CTI.Align));
                                    end;
                                 end if;
                                 -- Temporarily bind discriminant in Ctx.Vars so component default
                                 -- expressions referencing this discriminant read from FP!
                                 DVInfo.Kind       := VK_Local_Alloca;
                                 DVInfo.Pir_Name   := To_Unbounded_String (FP);
                                 DVInfo.Pir_Type   := CTI.Pir_Type;
                                 DVInfo.Ada_Type   := CT;
                                 DVInfo.Align      := CTI.Align;
                                 DVInfo.Owner_Subp := Ctx.Cur_Subp_Key;
                                 Ctx.Vars.Include (DK, DVInfo);
                                 Bound_Discs.Append (DK);
                              end;
                              FIdx := FIdx + 1;
                           end loop;
                        end;
                     end if;
                  end loop;
               end;
            end if;

            -- 2. Fixed record components (default expressions + recursive default init!)
            declare
               CL : constant TREE := D (SM_COMP_LIST, BT);
            begin
               if not Is_Void_Tree (CL) and then not Is_Void_Tree (D (AS_DECL_S, CL)) then
                  declare
                     DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                     Decl : TREE;
                  begin
                     while not IS_EMPTY (DL) loop
                        POP (DL, Decl);
                        if KIND (Decl) = DN_VARIABLE_DECL then
                           declare
                              NS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, Decl));
                              CID : TREE;
                              IE  : constant TREE := D (AS_EXP, Decl);
                           begin
                              while not IS_EMPTY (NS) loop
                                 POP (NS, CID);
                                 declare
                                    CID_K : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                    CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                                    RT_C : constant TREE := Resolve_Type_Spec (CT);
                                    CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                    FP  : constant String := Next_Reg (Ctx.E, "cfp");
                                    Has_SD_Save : Boolean := False;
                                    Prev_SD     : Saved_Disc_Info;
                                    Has_SR_Save : Boolean := False;
                                    Prev_SR     : Saved_Range_Info;
                                    SR_Key      : Unsigned_32 := 0;
                                 begin
                                    if not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Discs.Contains (CID_K) then
                                       if Ctx.Saved_Type_Discs.Contains (Tree_Key (RT_C)) then
                                          Has_SD_Save := True;
                                          Prev_SD := Ctx.Saved_Type_Discs.Element (Tree_Key (RT_C));
                                       end if;
                                       Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Ctx.Saved_Type_Discs.Element (CID_K));
                                    elsif not Is_Void_Tree (RT_C) and then Ctx.Saved_Type_Ranges.Contains (CID_K) then
                                       if KIND (RT_C) = DN_CONSTRAINED_ARRAY and then CTI.NDims >= 1 and then not Is_Void_Tree (CTI.Dims (1).Idx_Type) then
                                          SR_Key := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, CTI.Dims (1).Idx_Type)));
                                       else
                                          SR_Key := Tree_Key (RT_C);
                                       end if;
                                       if Ctx.Saved_Type_Ranges.Contains (SR_Key) then
                                          Has_SR_Save := True;
                                          Prev_SR := Ctx.Saved_Type_Ranges.Element (SR_Key);
                                       end if;
                                       Ctx.Saved_Type_Ranges.Include (SR_Key, Ctx.Saved_Type_Ranges.Element (CID_K));
                                    end if;
                                    if CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic then
                                       -- Allocate & initialize embedded @__ada_arr_desc with discriminants bound! (RC-8)
                                       Emit_Inst (Ctx.E,
                                         FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                         Dst_Ptr & ", " & Img (FIdx));
                                       declare
                                          ETI   : constant Type_Info := Get_Type_Info (Ctx, CTI.Elem_Type);
                                          ESz   : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                                          ND_A  : constant Natural := Natural'Max (1, Natural'Min (CTI.NDims, 4));
                                          BaseA : constant TREE := Resolve_Type_Spec (Base_Type_Spec (CT));
                                          BTI_A : constant Type_Info := Get_Type_Info (Ctx, BaseA);
                                          Firsts, Lasts : Str_Array_4;
                                          Tot64 : Unbounded_String;
                                          Byt   : constant String := Next_Reg (Ctx.E, "daby");
                                          Nz    : constant String := Next_Reg (Ctx.E, "danz");
                                          AlB   : constant String := Next_Reg (Ctx.E, "daal");
                                          DataP : Unbounded_String;
                                          P0    : constant String := Next_Reg (Ctx.E, "dap0");
                                       begin
                                          for D_I in 1 .. ND_A loop
                                             Emit_Index_Bounds (Ctx, CTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                                             -- Check non-null bounds belong to base array index subtype (c32114a, c32117a)
                                             if Ctx.Enable_Checks and then D_I <= BTI_A.NDims and then not Is_Void_Tree (BTI_A.Dims (D_I).Idx_Type) then
                                                declare
                                                   NE   : constant String := Next_Reg (Ctx.E, "dane");
                                                   L_C  : constant String := Next_Label (Ctx.E, "dac");
                                                   L_O  : constant String := Next_Label (Ctx.E, "dao");
                                                   BIT  : constant TREE := BTI_A.Dims (D_I).Idx_Type;
                                                   BITI : constant Type_Info := Get_Type_Info (Ctx, BIT);
                                                begin
                                                   Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (Firsts (D_I)) & ", " & To_String (Lasts (D_I)));
                                                   Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                                                   Emit_Label (Ctx.E, L_C);
                                                   declare
                                                      FC : constant String := Emit_Int_Convert (Ctx, To_String (Firsts (D_I)), "i32", To_String (BITI.Pir_Type));
                                                      LC : constant String := Emit_Int_Convert (Ctx, To_String (Lasts (D_I)), "i32", To_String (BITI.Pir_Type));
                                                   begin
                                                      Emit_Range_Check (Ctx, FC, To_String (BITI.Pir_Type), BIT);
                                                      Emit_Range_Check (Ctx, LC, To_String (BITI.Pir_Type), BIT);
                                                   end;
                                                   Emit_Term (Ctx.E, "jmp " & L_O);
                                                   Emit_Label (Ctx.E, L_O);
                                                end;
                                             end if;
                                             declare
                                                DL : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)));
                                             begin
                                                if D_I = 1 then Tot64 := To_Unbounded_String (DL);
                                                else
                                                   declare
                                                      M : constant String := Next_Reg (Ctx.E, "dam");
                                                   begin
                                                      Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & DL);
                                                      Tot64 := To_Unbounded_String (M);
                                                   end;
                                                end if;
                                             end;
                                          end loop;
                                          Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                                          Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                                          Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
                                          Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & FP & ", 0");
                                          Emit_Inst (Ctx.E, "store.ptr " & To_String (DataP) & ", " & P0 & ", align 8");
                                          for D_I in 1 .. ND_A loop
                                             declare
                                                FF : constant String := Next_Reg (Ctx.E, "daff");
                                                LF : constant String := Next_Reg (Ctx.E, "dalf");
                                             begin
                                                Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & FP & ", " & Img (2 * D_I - 1));
                                                Emit_Inst (Ctx.E, "store.i32 " & To_String (Firsts (D_I)) & ", " & FF & ", align 4");
                                                Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & FP & ", " & Img (2 * D_I));
                                                Emit_Inst (Ctx.E, "store.i32 " & To_String (Lasts (D_I)) & ", " & LF & ", align 4");
                                             end;
                                          end loop;
                                          if not Is_Void_Tree (IE) then
                                             if KIND (IE) = DN_AGGREGATE then
                                                Emit_Init_Composite (Ctx, To_String (DataP), CT, IE, Firsts, Lasts);
                                             else
                                                declare
                                                   SV  : constant Expr_Val := Emit_Expr (Ctx, IE, CT);
                                                   Tgt : Expr_Val;
                                                begin
                                                   Tgt.Kind := EK_Slice; Tgt.Reg := DataP; Tgt.Ada_Type := CT;
                                                   Tgt.NDims := ND_A; Tgt.First_Regs := Firsts; Tgt.Last_Regs := Lasts;
                                                   declare
                                                      IE_U : TREE := IE;
                                                   begin
                                                      while not Is_Void_Tree (IE_U) and then KIND (IE_U) = DN_PARENTHESIZED loop
                                                         IE_U := D (AS_EXP, IE_U);
                                                      end loop;
                                                      if not Is_Void_Tree (IE_U) and then KIND (IE_U) in DN_AGGREGATE | DN_STRING_LITERAL then
                                                         Emit_Array_Length_Check (Ctx, Tgt, SV, ND_A);
                                                      elsif Ctx.Enable_Checks then
                                                         for D_I in 1 .. ND_A loop
                                                            declare
                                                               SF, SL : Unbounded_String;
                                                               C1  : constant String := Next_Reg (Ctx.E, "dbf1");
                                                               C2  : constant String := Next_Reg (Ctx.E, "dbf2");
                                                               Bad : constant String := Next_Reg (Ctx.E, "dbfb");
                                                            begin
                                                               Get_Array_Dim_Bounds (Ctx, SV, D_I, SF, SL);
                                                               Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (SF) & ", " & To_String (Firsts (D_I)));
                                                               Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (SL) & ", " & To_String (Lasts (D_I)));
                                                               Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                                               Emit_Check_Fail_If (Ctx, Bad);
                                                            end;
                                                         end loop;
                                                      end if;
                                                   end;
                                                   Emit_Inst (Ctx.E, "memmove " & To_String (DataP) & ", " & To_String (SV.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                                                end;
                                             end if;
                                          elsif Is_Access_Type (CTI.Elem_Type) or else Is_Record_Type (CTI.Elem_Type) or else Is_Task_Type (CTI.Elem_Type) then
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
                                                   EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (DataP), IR);
                                                begin
                                                   Emit_Default_Init (Ctx, EP2, CTI.Elem_Type, Activate_Tasks => Activate_Tasks);
                                                end;
                                                Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                                                Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                                                Emit_Label (Ctx.E, L_E);
                                             end;
                                          end if;
                                       end;
                                    elsif not Is_Void_Tree (IE) then
                                       declare
                                          CK_D : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                          Has_FE_D : constant Boolean := Ctx.Fields.Contains (CK_D);
                                          FE_D : constant Field_Entry := (if Has_FE_D then Ctx.Fields.Element (CK_D) else (others => <>));
                                       begin
                                          if Has_FE_D and then (FE_D.In_Union or else FE_D.Has_Explicit_Offset) then
                                             declare
                                                Off_R : constant String := Next_Reg (Ctx.E, "dfo");
                                             begin
                                                Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE_D.Byte_Offset));
                                                Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                                             end;
                                          else
                                             Emit_Inst (Ctx.E,
                                               FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                               Dst_Ptr & ", " & Img (FIdx));
                                          end if;
                                          if CTI.Kind = TK_Scalar then
                                             declare
                                                V  : constant Expr_Val := Emit_Expr (Ctx, IE, CT);
                                                VR : constant String :=
                                                  Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (CTI.Pir_Type));
                                             begin
                                                Emit_Range_Check (Ctx, VR, To_String (CTI.Pir_Type), CT);
                                                Emit_Discriminant_Check (Ctx, VR, CT, Is_Access => True);
                                                if Has_FE_D and then Is_Bitfield (FE_D) then
                                                   Emit_Bitfield_Store (Ctx, Dst_Ptr, FE_D, VR, To_String (CTI.Pir_Type), CT);
                                                else
                                                   Emit_Inst (Ctx.E,
                                                     "store." & To_String (CTI.Pir_Type) & " " & VR &
                                                     ", " & FP & ", align " & Img (CTI.Align));
                                                end if;
                                             end;
                                          else
                                             declare
                                                IE_U : TREE := IE;
                                                Need_Exact : Boolean := False;
                                             begin
                                                while not Is_Void_Tree (IE_U) and then KIND (IE_U) = DN_PARENTHESIZED loop
                                                   IE_U := D (AS_EXP, IE_U);
                                                end loop;
                                                if CTI.Kind = TK_Constrained_Array
                                                  and then (Is_Void_Tree (IE_U) or else KIND (IE_U) not in DN_AGGREGATE | DN_STRING_LITERAL)
                                                then
                                                   Need_Exact := True;
                                                end if;
                                                Emit_Init_Composite (Ctx, FP, CT, IE, Exact_Bounds => Need_Exact);
                                             end;
                                          end if;
                                       end;
                                    elsif Is_Access_Type (Subst_Resolve_Type (Ctx, CT)) or else CTI.Kind = TK_Record
                                      or else Is_Task_Type (Subst_Resolve_Type (Ctx, CT))
                                      or else CTI.Kind = TK_Constrained_Array
                                    then
                                       declare
                                          CK_D : constant Unsigned_32 := Tree_Key (Canonical_Def (CID));
                                       begin
                                          if Ctx.Fields.Contains (CK_D)
                                            and then (Ctx.Fields.Element (CK_D).In_Union or else Ctx.Fields.Element (CK_D).Has_Explicit_Offset)
                                          then
                                             declare
                                                Off_R : constant String := Next_Reg (Ctx.E, "dfo");
                                             begin
                                                Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (Ctx.Fields.Element (CK_D).Byte_Offset));
                                                Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                                             end;
                                          else
                                             Emit_Inst (Ctx.E,
                                               FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " &
                                               Dst_Ptr & ", " & Img (FIdx));
                                          end if;
                                       end;
                                       Emit_Default_Init (Ctx, FP, CT, Activate_Tasks => Activate_Tasks);
                                    end if;
                                    if Has_SD_Save and then not Is_Void_Tree (RT_C) then
                                       Ctx.Saved_Type_Discs.Include (Tree_Key (RT_C), Prev_SD);
                                    end if;
                                    if Has_SR_Save and then SR_Key /= 0 then
                                       Ctx.Saved_Type_Ranges.Include (SR_Key, Prev_SR);
                                    end if;
                                 end;
                                 FIdx := FIdx + 1;
                              end loop;
                           end;
                        end if;
                     end loop;
                  end;
               end if;
               -- 2b. Variant part default initialization (recursive for nested variants: c48005a, c48012a, c52008b, c64108a, c95089a)
               declare
                  procedure Emit_Variant_Part_Default_Init (VP : TREE) is
                  begin
                     if Is_Void_Tree (VP) or else KIND (VP) /= DN_VARIANT_PART then
                        return;
                     end if;
                     declare
                        DDef  : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_NAME, VP))));
                        VS    : constant TREE := D (AS_VARIANT_S, VP);
                        VL    : SEQ_TYPE := (if not Is_Void_Tree (VS) then LIST (VS) else MAKE);
                        VN    : TREE;
                        L_End : constant String := Next_Label (Ctx.E, "vdi_e");
                     begin
                        if Ctx.Vars.Contains (Tree_Key (DDef)) then
                           declare
                              DV   : constant Var_Info := Ctx.Vars.Element (Tree_Key (DDef));
                              DVal : constant String := Next_Reg (Ctx.E, "vdv");
                              D64  : Unbounded_String;
                           begin
                              Emit_Inst (Ctx.E, DVal & " = load." & To_String (DV.Pir_Type) & " " & To_String (DV.Pir_Name) & ", align " & Img (DV.Align));
                              D64 := To_Unbounded_String (Emit_Int_Convert (Ctx, DVal, To_String (DV.Pir_Type), "i64"));
                              while not IS_EMPTY (VL) loop
                                 POP (VL, VN);
                                 if KIND (VN) = DN_VARIANT then
                                    declare
                                       CS    : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN));
                                       Ch    : TREE;
                                       Cond  : Unbounded_String;
                                       Is_Ot : Boolean := False;
                                       L_Hit : constant String := Next_Label (Ctx.E, "vdi_h");
                                       L_Nxt : constant String := Next_Label (Ctx.E, "vdi_n");
                                    begin
                                       while not IS_EMPTY (CS) loop
                                          POP (CS, Ch);
                                          if KIND (Ch) = DN_CHOICE_OTHERS then
                                             Is_Ot := True;
                                          elsif KIND (Ch) = DN_CHOICE_EXP then
                                             declare
                                                CE  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch));
                                                C64 : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i64");
                                                Eq  : constant String := Next_Reg (Ctx.E, "vde");
                                             begin
                                                Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & To_String (D64) & ", " & C64);
                                                if Length (Cond) = 0 then Cond := To_Unbounded_String (Eq);
                                                else
                                                   declare
                                                      O2 : constant String := Next_Reg (Ctx.E, "vdo");
                                                   begin
                                                      Emit_Inst (Ctx.E, O2 & " = or.i8 " & To_String (Cond) & ", " & Eq);
                                                      Cond := To_Unbounded_String (O2);
                                                   end;
                                                end if;
                                             end;
                                          elsif KIND (Ch) = DN_CHOICE_RANGE then
                                             declare
                                                RT, RL, RH : Unbounded_String;
                                                L64, H64 : Unbounded_String;
                                                G1 : constant String := Next_Reg (Ctx.E, "vdg");
                                                L1 : constant String := Next_Reg (Ctx.E, "vdl");
                                                InR : constant String := Next_Reg (Ctx.E, "vdr");
                                             begin
                                                Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                                L64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i64"));
                                                H64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i64"));
                                                Emit_Inst (Ctx.E, G1 & " = cmp_ge_s.i64 " & To_String (D64) & ", " & To_String (L64));
                                                Emit_Inst (Ctx.E, L1 & " = cmp_le_s.i64 " & To_String (D64) & ", " & To_String (H64));
                                                Emit_Inst (Ctx.E, InR & " = and.i8 " & G1 & ", " & L1);
                                                if Length (Cond) = 0 then Cond := To_Unbounded_String (InR);
                                                else
                                                   declare
                                                      O2 : constant String := Next_Reg (Ctx.E, "vdo");
                                                   begin
                                                      Emit_Inst (Ctx.E, O2 & " = or.i8 " & To_String (Cond) & ", " & InR);
                                                      Cond := To_Unbounded_String (O2);
                                                   end;
                                                end if;
                                             end;
                                          end if;
                                       end loop;
                                       if Is_Ot then
                                          Emit_Term (Ctx.E, "jmp " & L_Hit);
                                       elsif Length (Cond) > 0 then
                                          Emit_Term (Ctx.E, "br " & To_String (Cond) & ", " & L_Hit & ", " & L_Nxt);
                                       else
                                          Emit_Term (Ctx.E, "jmp " & L_Nxt);
                                       end if;
                                       Emit_Label (Ctx.E, L_Hit);
                                       -- Initialize variant's components using Ctx.Fields byte offsets
                                       declare
                                          VCL : constant TREE := D (AS_COMP_LIST, VN);
                                       begin
                                          if not Is_Void_Tree (VCL) then
                                             if not Is_Void_Tree (D (AS_DECL_S, VCL)) then
                                                declare
                                                   VDL : SEQ_TYPE := LIST (D (AS_DECL_S, VCL));
                                                   VDecl : TREE;
                                                begin
                                                   while not IS_EMPTY (VDL) loop
                                                      POP (VDL, VDecl);
                                                      if KIND (VDecl) = DN_VARIABLE_DECL then
                                                         declare
                                                            VNS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, VDecl));
                                                            VCID : TREE;
                                                            VIE  : constant TREE := D (AS_EXP, VDecl);
                                                         begin
                                                            while not IS_EMPTY (VNS) loop
                                                               POP (VNS, VCID);
                                                               declare
                                                                  VCT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, VCID));
                                                                  VCTI : constant Type_Info := Get_Type_Info (Ctx, VCT);
                                                                  VCK  : constant Unsigned_32 := Tree_Key (Canonical_Def (VCID));
                                                               begin
                                                                  if Ctx.Fields.Contains (VCK) then
                                                                     declare
                                                                        FE  : constant Field_Entry := Ctx.Fields.Element (VCK);
                                                                        FP  : constant String := Next_Reg (Ctx.E, "vfp");
                                                                     begin
                                                                        if FE.In_Union or else FE.Has_Explicit_Offset then
                                                                           declare
                                                                              Off_R : constant String := Next_Reg (Ctx.E, "vfo");
                                                                           begin
                                                                              Emit_Inst (Ctx.E, Off_R & " = const.i64 " & Img (FE.Byte_Offset));
                                                                              Emit_Inst (Ctx.E, FP & " = ptr_add.i64 " & Dst_Ptr & ", " & Off_R & " [inbounds]");
                                                                           end;
                                                                        else
                                                                           Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " & Dst_Ptr & ", " & Img (FE.Field_Idx));
                                                                        end if;
                                                                        if VCTI.Kind = TK_Constrained_Array and then VCTI.Is_Dynamic then
                                                                           declare
                                                                              ETI   : constant Type_Info := Get_Type_Info (Ctx, VCTI.Elem_Type);
                                                                              ESz   : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                                                                              ND_A  : constant Natural := Natural'Max (1, Natural'Min (VCTI.NDims, 4));
                                                                              Firsts, Lasts : Str_Array_4;
                                                                              Tot64 : Unbounded_String;
                                                                              Byt   : constant String := Next_Reg (Ctx.E, "vaby");
                                                                              Nz    : constant String := Next_Reg (Ctx.E, "vanz");
                                                                              AlB   : constant String := Next_Reg (Ctx.E, "vaal");
                                                                              DataP : Unbounded_String;
                                                                              P0    : constant String := Next_Reg (Ctx.E, "vap0");
                                                                           begin
                                                                              for D_I in 1 .. ND_A loop
                                                                                 Emit_Index_Bounds (Ctx, VCTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
                                                                                 if Ctx.Enable_Checks then
                                                                                    declare
                                                                                       BA  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (VCT));
                                                                                       BTI : constant Type_Info := (if not Is_Void_Tree (BA) and then KIND (BA) = DN_ARRAY then Get_Type_Info (Ctx, BA) else VCTI);
                                                                                    begin
                                                                                       if D_I <= BTI.NDims and then not Is_Void_Tree (BTI.Dims (D_I).Idx_Type) then
                                                                                          declare
                                                                                             NE  : constant String := Next_Reg (Ctx.E, "vane");
                                                                                             L_C : constant String := Next_Label (Ctx.E, "vac");
                                                                                             L_O : constant String := Next_Label (Ctx.E, "vao");
                                                                                             BIT : constant TREE := BTI.Dims (D_I).Idx_Type;
                                                                                             BITI : constant Type_Info := Get_Type_Info (Ctx, BIT);
                                                                                          begin
                                                                                             Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (Firsts (D_I)) & ", " & To_String (Lasts (D_I)));
                                                                                             Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                                                                                             Emit_Label (Ctx.E, L_C);
                                                                                             declare
                                                                                                FC : constant String := Emit_Int_Convert (Ctx, To_String (Firsts (D_I)), "i32", To_String (BITI.Pir_Type));
                                                                                                LC : constant String := Emit_Int_Convert (Ctx, To_String (Lasts (D_I)), "i32", To_String (BITI.Pir_Type));
                                                                                             begin
                                                                                                Emit_Range_Check (Ctx, FC, To_String (BITI.Pir_Type), BIT);
                                                                                                Emit_Range_Check (Ctx, LC, To_String (BITI.Pir_Type), BIT);
                                                                                             end;
                                                                                             Emit_Term (Ctx.E, "jmp " & L_O);
                                                                                             Emit_Label (Ctx.E, L_O);
                                                                                          end;
                                                                                       end if;
                                                                                    end;
                                                                                 end if;
                                                                                 declare
                                                                                    DL : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)));
                                                                                 begin
                                                                                    if D_I = 1 then Tot64 := To_Unbounded_String (DL);
                                                                                    else
                                                                                       declare
                                                                                          M : constant String := Next_Reg (Ctx.E, "vam");
                                                                                       begin
                                                                                          Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & DL);
                                                                                          Tot64 := To_Unbounded_String (M);
                                                                                       end;
                                                                                    end if;
                                                                                 end;
                                                                              end loop;
                                                                              Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                                                                              Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                                                                              Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                                                                              Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
                                                                              Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & FP & ", 0");
                                                                              Emit_Inst (Ctx.E, "store.ptr " & To_String (DataP) & ", " & P0 & ", align 8");
                                                                              for D_I in 1 .. ND_A loop
                                                                                 declare
                                                                                    FF : constant String := Next_Reg (Ctx.E, "vaff");
                                                                                    LF : constant String := Next_Reg (Ctx.E, "valf");
                                                                                 begin
                                                                                    Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & FP & ", " & Img (2 * D_I - 1));
                                                                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (Firsts (D_I)) & ", " & FF & ", align 4");
                                                                                    Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & FP & ", " & Img (2 * D_I));
                                                                                    Emit_Inst (Ctx.E, "store.i32 " & To_String (Lasts (D_I)) & ", " & LF & ", align 4");
                                                                                 end;
                                                                              end loop;
                                                                              if not Is_Void_Tree (VIE) then
                                                                                 Emit_Init_Composite (Ctx, To_String (DataP), VCT, VIE, Firsts, Lasts);
                                                                              end if;
                                                                           end;
                                                                        elsif not Is_Void_Tree (VIE) then
                                                                           if VCTI.Kind = TK_Scalar then
                                                                              declare
                                                                                 VV : constant Expr_Val := Emit_Expr (Ctx, VIE, VCT);
                                                                                 VR : constant String := Emit_Scalar_Coerce (Ctx, To_String (VV.Reg), To_String (VV.Pir_Type), To_String (VCTI.Pir_Type));
                                                                              begin
                                                                                 Emit_Range_Check (Ctx, VR, To_String (VCTI.Pir_Type), VCT);
                                                                                 Emit_Discriminant_Check (Ctx, VR, VCT, Is_Access => True);
                                                                                 Emit_Inst (Ctx.E, "store." & To_String (VCTI.Pir_Type) & " " & VR & ", " & FP & ", align " & Img (VCTI.Align));
                                                                              end;
                                                                           else
                                                                              Emit_Init_Composite (Ctx, FP, VCT, VIE);
                                                                           end if;
                                                                        elsif Is_Access_Type (Subst_Resolve_Type (Ctx, VCT)) or else VCTI.Kind = TK_Record
                                                                          or else Is_Task_Type (Subst_Resolve_Type (Ctx, VCT))
                                                                          or else VCTI.Kind = TK_Constrained_Array
                                                                        then
                                                                           Emit_Default_Init (Ctx, FP, VCT, Activate_Tasks => Activate_Tasks);
                                                                        end if;
                                                                     end;
                                                                  end if;
                                                               end;
                                                            end loop;
                                                         end;
                                                      end if;
                                                   end loop;
                                                end;
                                             end if;
                                             if not Is_Void_Tree (D (AS_VARIANT_PART, VCL)) then
                                                Emit_Variant_Part_Default_Init (D (AS_VARIANT_PART, VCL));
                                             end if;
                                          end if;
                                       end;
                                       Emit_Term (Ctx.E, "jmp " & L_End);
                                       Emit_Label (Ctx.E, L_Nxt);
                                    end;
                                 end if;
                              end loop;
                              Emit_Term (Ctx.E, "jmp " & L_End);
                              Emit_Label (Ctx.E, L_End);
                           end;
                        end if;
                     end;
                  end Emit_Variant_Part_Default_Init;
               begin
                  if not Is_Void_Tree (CL) and then not Is_Void_Tree (D (AS_VARIANT_PART, CL)) then
                     Emit_Variant_Part_Default_Init (D (AS_VARIANT_PART, CL));
                  end if;
               end;
            end;

            -- Clean up temporary discriminant bindings
            for DK of Bound_Discs loop
               if Ctx.Vars.Contains (DK) then
                  Ctx.Vars.Delete (DK);
               end if;
            end loop;
         end;

      elsif DTI.Kind = TK_Constrained_Array and then DTI.Is_Dynamic then
         -- Embedded @__ada_arr_desc for a dynamically-constrained array element (c32001b, c42007g, c43205k, c43214f, cd2a81a)
         declare
            ETI   : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
            Is_DE : constant Boolean := ETI.Kind = TK_Constrained_Array and then ETI.Is_Dynamic;
            EAl   : constant Natural := (if Is_DE then 8 else Natural'Max (1, ETI.Align));
            ESz   : constant Natural := (if Is_DE then 40 else Natural'Max (1, ((ETI.Byte_Size + EAl - 1) / EAl) * EAl));
            ND_A  : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, 4));
            Firsts, Lasts : Str_Array_4;
            Tot64 : Unbounded_String;
            Byt   : constant String := Next_Reg (Ctx.E, "eaby");
            Nz    : constant String := Next_Reg (Ctx.E, "eanz");
            AlB   : constant String := Next_Reg (Ctx.E, "eaal");
            DataP : Unbounded_String;
            P0    : constant String := Next_Reg (Ctx.E, "eap0");
         begin
            for D_I in 1 .. ND_A loop
               Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, Firsts (D_I), Lasts (D_I));
               declare
                  DL : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)));
               begin
                  if D_I = 1 then Tot64 := To_Unbounded_String (DL);
                  else
                     declare
                        M : constant String := Next_Reg (Ctx.E, "eam");
                     begin
                        Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & DL);
                        Tot64 := To_Unbounded_String (M);
                     end;
                  end if;
               end;
            end loop;
            Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
            Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
            Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (EAl));
            Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", AlB, "ptr", DataP);
            Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & Dst_Ptr & ", 0");
            Emit_Inst (Ctx.E, "store.ptr " & To_String (DataP) & ", " & P0 & ", align 8");
            for D_I in 1 .. ND_A loop
               declare
                  FF : constant String := Next_Reg (Ctx.E, "eaff");
                  LF : constant String := Next_Reg (Ctx.E, "ealf");
               begin
                  Emit_Inst (Ctx.E, FF & " = field_ptr @__ada_arr_desc, " & Dst_Ptr & ", " & Img (2 * D_I - 1));
                  Emit_Inst (Ctx.E, "store.i32 " & To_String (Firsts (D_I)) & ", " & FF & ", align 4");
                  Emit_Inst (Ctx.E, LF & " = field_ptr @__ada_arr_desc, " & Dst_Ptr & ", " & Img (2 * D_I));
                  Emit_Inst (Ctx.E, "store.i32 " & To_String (Lasts (D_I)) & ", " & LF & ", align 4");
               end;
            end loop;
            if Is_Access_Type (Subst_Resolve_Type (Ctx, DTI.Elem_Type)) or else ETI.Kind = TK_Record
              or else Is_Task_Type (Subst_Resolve_Type (Ctx, DTI.Elem_Type)) or else ETI.Kind = TK_Constrained_Array
            then
               declare
                  L_H : constant String := Next_Label (Ctx.E, "eadi_h");
                  L_B : constant String := Next_Label (Ctx.E, "eadi_b");
                  L_E : constant String := Next_Label (Ctx.E, "eadi_e");
                  IR  : constant String := Next_Reg (Ctx.E, "eadi");
                  Cnd : constant String := Next_Reg (Ctx.E, "eadc");
                  Nx  : constant String := Next_Reg (Ctx.E, "eadn");
               begin
                  Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                  Emit_Label (Ctx.E, L_H, IR & ": i64");
                  Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                  Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                  Emit_Label (Ctx.E, L_B);
                  declare
                     EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, To_String (DataP), IR);
                  begin
                     Emit_Default_Init (Ctx, EP2, DTI.Elem_Type, Activate_Tasks => Activate_Tasks);
                  end;
                  Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                  Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                  Emit_Label (Ctx.E, L_E);
               end;
            end if;
         end;

      elsif DTI.Kind = TK_Constrained_Array and then not DTI.Is_Dynamic then
         -- Only emit default init loop for arrays whose elements need default init (access / record / nested array)
         declare
            ETI : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
         begin
            if Is_Access_Type (DTI.Elem_Type) or else Is_Record_Type (DTI.Elem_Type)
              or else Is_Task_Type (DTI.Elem_Type)
              or else ETI.Kind = TK_Constrained_Array
            then
               declare
                  N_Tot : Integer_64 := 1;
               begin
                  for D_Idx in 1 .. DTI.NDims loop
                     if DTI.Dims (D_Idx).Last >= DTI.Dims (D_Idx).First then
                        N_Tot := N_Tot * (DTI.Dims (D_Idx).Last - DTI.Dims (D_Idx).First + 1);
                     else
                        N_Tot := 0;
                     end if;
                  end loop;
                  if N_Tot > 0 and then N_Tot <= 16_777_216 then
                     declare
                        L_Hdr  : constant String := Next_Label (Ctx.E, "di_h");
                        L_Bdy  : constant String := Next_Label (Ctx.E, "di_b");
                        L_Done : constant String := Next_Label (Ctx.E, "di_e");
                        I_Reg  : constant String := Next_Reg (Ctx.E, "di");
                        Cond   : constant String := Next_Reg (Ctx.E, "dc");
                        EP     : constant String := Next_Reg (Ctx.E, "dep");
                        Nx     : constant String := Next_Reg (Ctx.E, "din");
                     begin
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(0)");
                        Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                        Emit_Inst (Ctx.E, Cond & " = cmp_lt_s.i64 " & I_Reg & ", " & Img (Long_Long_Integer (N_Tot)));
                        Emit_Term (Ctx.E, "br " & Cond & ", " & L_Bdy & ", " & L_Done);
                        Emit_Label (Ctx.E, L_Bdy);
                        declare
                           EP2 : constant String := Emit_Elem_Ptr (Ctx, ETI, Dst_Ptr, I_Reg);
                        begin
                           Emit_Default_Init (Ctx, EP2, DTI.Elem_Type, Activate_Tasks => Activate_Tasks);
                        end;
                        Emit_Inst (Ctx.E, Nx & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                        Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nx & ")");
                        Emit_Label (Ctx.E, L_Done);
                     end;
                  end if;
               end;
            end if;
         end;
      end if;
   end Emit_Default_Init;

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
                     Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L1) & ", " & To_String (F1));
                     Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                     Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                     Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                     Emit_Inst (Ctx.E, C6 & " = sext.i64 " & Cl);
                     Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", C6 & ", 1", "ptr", Buf_U);
                     Emit_Term (Ctx.E, "jmp " & L_Hdr & "(0)");
                     Emit_Label (Ctx.E, L_Hdr, IR & ": i64");
                     Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & C6);
                     Emit_Term (Ctx.E, "br " & Cnd & ", " & L_Bdy & ", " & L_End);
                     Emit_Label (Ctx.E, L_Bdy);
                     Emit_Inst (Ctx.E, SP & " = ptr_add.i64 " & To_String (V.Reg) & ", " & IR & " [inbounds]");
                     Emit_Inst (Ctx.E, DP & " = ptr_add.i64 " & To_String (Buf_U) & ", " & IR & " [inbounds]");
                     Emit_Inst (Ctx.E, BV & " = load.i8 " & SP & ", align 1");
                     Emit_Inst (Ctx.E, NV & " = xor.i8 " & BV & ", 1");
                     Emit_Inst (Ctx.E, "store.i8 " & NV & ", " & DP & ", align 1");
                     Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                     Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Nx & ")");
                     Emit_Label (Ctx.E, L_End);
                     Res.Kind           := EK_Slice;
                     Res.Reg            := Buf_U;
                     Res.Pir_Type       := To_Unbounded_String ("ptr");
                     Res.Ada_Type       := Res_Type;
                     Res.Align          := 1;
                     Res.NDims          := 1;
                     Res.First_Regs (1) := F1;
                     Res.Last_Regs (1)  := L1;
                     return Res;
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
                  if Ctx.Enable_Checks then
                     declare
                        Ovf : constant String := Next_Reg (Ctx.E, "ov");
                     begin
                        Emit_Inst (Ctx.E, R & ", " & Ovf & " = sub_ovf_s." & PT & " 0, " & To_String (V.Reg));
                        Emit_Check_Fail_If (Ctx, Ovf);
                     end;
                  else
                     Emit_Inst (Ctx.E, R & " = sub." & PT & " 0, " & To_String (V.Reg) & " [unsafe_s]");
                  end if;
                  Res.Reg := To_Unbounded_String (R);
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
                  if Ctx.Enable_Checks then
                     declare
                        Is_Min : constant String := Next_Reg (Ctx.E, "amin");
                     begin
                        Emit_Inst (Ctx.E, Is_Min & " = cmp_eq." & PT & " " & To_String (V.Reg) & ", " &
                          Img (Min_Signed_For_Pir (PT)));
                        Emit_Check_Fail_If (Ctx, Is_Min);
                     end;
                  end if;
                  declare
                     Neg : constant String := Next_Reg (Ctx.E, "an");
                     Cnd : constant String := Next_Reg (Ctx.E, "ac");
                  begin
                     Emit_Inst (Ctx.E, Neg & " = sub." & PT & " 0, " & To_String (V.Reg));
                     Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s." & PT & " " & To_String (V.Reg) & ", 0");
                     Emit_Inst (Ctx.E, R & " = select." & PT & " " & Cnd & ", " & Neg & ", " & To_String (V.Reg));
                     Res.Reg := To_Unbounded_String (R);
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
            EAl : constant Natural := Natural'Max (1, ETI.Align);
            ESz : constant Natural := Natural'Max (1, ((ETI.Byte_Size + EAl - 1) / EAl) * EAl);
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
                        LV  : constant String := Emit_Scalar_Coerce (Ctx, To_String (L.Reg), To_String (L.Pir_Type), To_String (ETI.Pir_Type));
                     begin
                        Emit_Range_Check (Ctx, LV, To_String (ETI.Pir_Type), Comp_T);
                        Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (ETI.Pir_Type) & ", align " & Img (EAl));
                        Emit_Inst (Ctx.E, "store." & To_String (ETI.Pir_Type) & " " & LV & ", " & Tmp & ", align " & Img (EAl));
                        L_Ptr := To_Unbounded_String (Tmp);
                     end;
                  else
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
                        RV  : constant String := Emit_Scalar_Coerce (Ctx, To_String (R_V.Reg), To_String (R_V.Pir_Type), To_String (ETI.Pir_Type));
                     begin
                        Emit_Range_Check (Ctx, RV, To_String (ETI.Pir_Type), Comp_T);
                        Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (ETI.Pir_Type) & ", align " & Img (EAl));
                        Emit_Inst (Ctx.E, "store." & To_String (ETI.Pir_Type) & " " & RV & ", " & Tmp & ", align " & Img (EAl));
                        R_Ptr := To_Unbounded_String (Tmp);
                     end;
                  else
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
               F64    : constant String := Next_Reg (Ctx.E, "cf64");
               Last64 : constant String := Next_Reg (Ctx.E, "cl64");
               Last32 : constant String := Next_Reg (Ctx.E, "cl32");
            begin
               Emit_Inst (Ctx.E, Tot64 & " = add.i64 " & To_String (L_Len) & ", " & To_String (R_Len));
               Emit_Inst (Ctx.E, Bytes & " = mul.i64 " & Tot64 & ", " & Img (ESz));
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Bytes & ", " & Img (ETI.Align), "ptr", Buf_U);
               Emit_Inst (Ctx.E, LBy & " = mul.i64 " & To_String (L_Len) & ", " & Img (ESz));
               Emit_Inst (Ctx.E, "memcpy " & To_String (Buf_U) & ", " & To_String (L_Ptr) & ", " & LBy & ", align " & Img (ETI.Align));
               Emit_Inst (Ctx.E, Dst2 & " = ptr_add.i64 " & To_String (Buf_U) & ", " & LBy & " [inbounds]");
               Emit_Inst (Ctx.E, RBy & " = mul.i64 " & To_String (R_Len) & ", " & Img (ESz));
               Emit_Inst (Ctx.E, "memcpy " & Dst2 & ", " & To_String (R_Ptr) & ", " & RBy & ", align " & Img (ETI.Align));
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
                     Emit_Inst (Ctx.E, Pick_F & " = select.i32 " & L_Non_Null & ", " & To_String (LF_R) & ", " & To_String (Alt_F));
                     Res_F := To_Unbounded_String (Pick_F);
                     Has_L_Non_Null := True;
                     L_NN_Reg := To_Unbounded_String (L_Non_Null);
                  end;
               end if;
               Emit_Inst (Ctx.E, F64 & " = sext.i64 " & To_String (Res_F));
               declare
                  Tm1 : constant String := Next_Reg (Ctx.E, "ctm1");
               begin
                  Emit_Inst (Ctx.E, Tm1 & " = sub.i64 " & Tot64 & ", 1");
                  Emit_Inst (Ctx.E, Last64 & " = add.i64 " & F64 & ", " & Tm1);
               end;
               if Ctx.Enable_Checks then
                  declare
                     H64 : constant String := Next_Reg (Ctx.E, "ch64");
                     NZ  : constant String := Next_Reg (Ctx.E, "cnz");
                     GT  : constant String := Next_Reg (Ctx.E, "cgt");
                     Bad : constant String := Next_Reg (Ctx.E, "cbad");
                  begin
                     Emit_Inst (Ctx.E, H64 & " = sext.i64 " & To_String (Idx_Hi));
                     Emit_Inst (Ctx.E, NZ  & " = cmp_gt_s.i64 " & Tot64 & ", 0");
                     Emit_Inst (Ctx.E, GT  & " = cmp_gt_s.i64 " & Last64 & ", " & H64);
                     Emit_Inst (Ctx.E, Bad & " = and.i8 " & NZ & ", " & GT);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;
               Emit_Inst (Ctx.E, Last32 & " = trunc.i32 " & Last64);
               declare
                  Final_L32 : Unbounded_String := To_Unbounded_String (Last32);
               begin
                  if Has_L_Non_Null and then not R_Is_Elem and then Length (Saved_RL_R) > 0 then
                     declare
                        PL : constant String := Next_Reg (Ctx.E, "cpl");
                     begin
                        Emit_Inst (Ctx.E, PL & " = select.i32 " & To_String (L_NN_Reg) & ", " & Last32 & ", " & To_String (Saved_RL_R));
                        Final_L32 := To_Unbounded_String (PL);
                     end;
                  end if;
                  Res.Kind           := EK_Slice;
                  Res.Reg            := Buf_U;
                  Res.Pir_Type       := To_Unbounded_String ("ptr");
                  Res.Ada_Type       := BT;
                  Res.Align          := ETI.Align;
                  Res.NDims          := 1;
                  Res.First_Regs (1) := Res_F;
                  Res.Last_Regs (1)  := Final_L32;
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
                  Depth          : Natural := 0) return String
               is
                  RT  : constant TREE := Subst_Resolve_Type (Ctx, Val_T);
                  VTI : constant Type_Info := Get_Type_Info (Ctx, RT);
               begin
                  if Depth > 8 or else VTI.Kind = TK_Void then
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
                        ND  : constant Natural := Natural'Max (1, Natural'Min (VTI.NDims, 4));
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
                     begin
                        Emit_Inst (Ctx.E, V1 & " = load." & PT_S & " " & P1_Reg & ", align " & Img (Natural'Max (1, VTI.Align)));
                        Emit_Inst (Ctx.E, V2 & " = load." & PT_S & " " & P2_Reg & ", align " & Img (Natural'Max (1, VTI.Align)));
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
                                 begin
                                    Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (CTI.Pir_Type) & " " & V1 & ", " & V2);
                                    FEq := To_Unbounded_String (Eq);
                                 end;
                              else
                                 Emit_Rec_FP (P1_Reg, FE, FP1);
                                 Emit_Rec_FP (P2_Reg, FE, FP2);
                                 FEq := To_Unbounded_String (Emit_Val_Equal (FP1, FP2, CT,
                                   Is_Dyn_Desc => (CTI.Kind = TK_Constrained_Array and then CTI.Is_Dynamic),
                                   Depth       => Depth + 1));
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
                                       DP   : constant String := Next_Reg (Ctx.E, "vdp");
                                       DV   : constant String := Next_Reg (Ctx.E, "vdv");
                                       DV64 : Unbounded_String;
                                       VL   : SEQ_TYPE := LIST (D (AS_VARIANT_S, VP));
                                       VN   : TREE;
                                       L_VEnd : constant String := Next_Label (Ctx.E, "vpe");
                                    begin
                                       Emit_Rec_FP (P1_Reg, DFE, DP);
                                       Emit_Inst (Ctx.E, DV & " = load." & To_String (DTI.Pir_Type) & " " & DP & ", align " & Img (Natural'Max (1, DTI.Align)));
                                       DV64 := To_Unbounded_String (Emit_Int_Convert (Ctx, DV, To_String (DTI.Pir_Type), "i64"));
                                       while not IS_EMPTY (VL) loop
                                          POP (VL, VN);
                                          if KIND (VN) = DN_VARIANT then
                                             declare
                                                CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, VN));
                                                Ch : TREE;
                                                Hit : Unbounded_String;
                                                Has_Others : Boolean := False;
                                                L_Hit : constant String := Next_Label (Ctx.E, "vh");
                                                L_Nxt : constant String := Next_Label (Ctx.E, "vn");
                                             begin
                                                while not IS_EMPTY (CS) loop
                                                   POP (CS, Ch);
                                                   if KIND (Ch) = DN_CHOICE_OTHERS then
                                                      Has_Others := True;
                                                   elsif KIND (Ch) = DN_CHOICE_EXP then
                                                      declare
                                                         CV  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch), DT_A);
                                                         C64 : constant String := Emit_Int_Convert (Ctx, To_String (CV.Reg), To_String (CV.Pir_Type), "i64");
                                                         Eq  : constant String := Next_Reg (Ctx.E, "vce");
                                                      begin
                                                         Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & To_String (DV64) & ", " & C64);
                                                         if Length (Hit) = 0 then
                                                            Hit := To_Unbounded_String (Eq);
                                                         else
                                                            declare
                                                               O : constant String := Next_Reg (Ctx.E, "vco");
                                                            begin
                                                               Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & Eq);
                                                               Hit := To_Unbounded_String (O);
                                                            end;
                                                         end if;
                                                      end;
                                                   elsif KIND (Ch) = DN_CHOICE_RANGE then
                                                      declare
                                                         RT2, LoR, HiR : Unbounded_String;
                                                         Lo64, Hi64 : Unbounded_String;
                                                         Ge : constant String := Next_Reg (Ctx.E, "vge");
                                                         Le : constant String := Next_Reg (Ctx.E, "vle");
                                                         InR : constant String := Next_Reg (Ctx.E, "vin");
                                                      begin
                                                         Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT2, LoR, HiR);
                                                         Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LoR), To_String (RT2), "i64"));
                                                         Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (HiR), To_String (RT2), "i64"));
                                                         Emit_Inst (Ctx.E, Ge & " = cmp_ge_s.i64 " & To_String (DV64) & ", " & To_String (Lo64));
                                                         Emit_Inst (Ctx.E, Le & " = cmp_le_s.i64 " & To_String (DV64) & ", " & To_String (Hi64));
                                                         Emit_Inst (Ctx.E, InR & " = and.i8 " & Ge & ", " & Le);
                                                         if Length (Hit) = 0 then
                                                            Hit := To_Unbounded_String (InR);
                                                         else
                                                            declare
                                                               O : constant String := Next_Reg (Ctx.E, "vco");
                                                            begin
                                                               Emit_Inst (Ctx.E, O & " = or.i8 " & To_String (Hit) & ", " & InR);
                                                               Hit := To_Unbounded_String (O);
                                                            end;
                                                         end if;
                                                      end;
                                                   end if;
                                                end loop;
                                                if Has_Others then
                                                   Check_Comp_List (D (AS_COMP_LIST, VN));
                                                   Emit_Term (Ctx.E, "jmp " & L_VEnd);
                                                   Emit_Label (Ctx.E, L_Nxt);
                                                elsif Length (Hit) > 0 then
                                                   Emit_Term (Ctx.E, "br " & To_String (Hit) & ", " & L_Hit & ", " & L_Nxt);
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
                        BR : constant TREE := Resolve_Type_Spec (Base_Type_Spec (LT));
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
                     ESz : constant Natural := (if Is_DE then 40 else Natural'Max (1, ((ETI.Byte_Size + EAl - 1) / EAl) * EAl));
                     ND  : constant Natural := Natural'Max (1, Natural'Min (LTI.NDims, 4));
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
                     if ETI.Kind in TK_Record | TK_Constrained_Array or else Is_Float_Type (LTI.Elem_Type) then
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
               Emit_Inst (Ctx.E, D1 & " = sub.i32 " & To_String (L1) & ", " & To_String (F1));
               Emit_Inst (Ctx.E, N1 & " = add.i32 " & D1 & ", 1");
               Emit_Inst (Ctx.E, P1 & " = cmp_gt_s.i32 " & N1 & ", 0");
               Emit_Inst (Ctx.E, C1 & " = select.i32 " & P1 & ", " & N1 & ", 0");
               Emit_Inst (Ctx.E, D2 & " = sub.i32 " & To_String (L2) & ", " & To_String (F2));
               Emit_Inst (Ctx.E, N2 & " = add.i32 " & D2 & ", 1");
               Emit_Inst (Ctx.E, P2 & " = cmp_gt_s.i32 " & N2 & ", 0");
               Emit_Inst (Ctx.E, C2 & " = select.i32 " & P2 & ", " & N2 & ", 0");
               if Ctx.Enable_Checks then
                  Emit_Inst (Ctx.E, Lne & " = cmp_ne.i32 " & C1 & ", " & C2);
                  Emit_Check_Fail_If (Ctx, Lne);
               end if;
               Emit_Inst (Ctx.E, N64 & " = sext.i64 " & C1);
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", N64 & ", 1", "ptr", Buf);
               Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
               Emit_Label (Ctx.E, L_H, IR & ": i64");
               Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & N64);
               Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
               Emit_Label (Ctx.E, L_B);
               Emit_Inst (Ctx.E, Q1 & " = ptr_add.i64 " & To_String (L.Reg) & ", " & IR & " [inbounds]");
               Emit_Inst (Ctx.E, Q2 & " = ptr_add.i64 " & To_String (R_V.Reg) & ", " & IR & " [inbounds]");
               Emit_Inst (Ctx.E, QD & " = ptr_add.i64 " & To_String (Buf) & ", " & IR & " [inbounds]");
               Emit_Inst (Ctx.E, B1 & " = load.i8 " & Q1 & ", align 1");
               Emit_Inst (Ctx.E, B2 & " = load.i8 " & Q2 & ", align 1");
               Emit_Inst (Ctx.E, BO & " = " & Opc & " " & B1 & ", " & B2);
               Emit_Inst (Ctx.E, "store.i8 " & BO & ", " & QD & ", align 1");
               Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
               Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
               Emit_Label (Ctx.E, L_E);
               Res.Kind           := EK_Slice;
               Res.Reg            := Buf;
               Res.Pir_Type       := To_Unbounded_String ("ptr");
               Res.Ada_Type       := Res_Type;
               Res.Align          := 1;
               Res.NDims          := 1;
               Res.First_Regs (1) := F1;
               Res.Last_Regs (1)  := L1;
               return Res;
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
            begin
               Get_Array_Dim_Bounds (Ctx, L, 1, F1, L1);
               Get_Array_Dim_Bounds (Ctx, R_V, 1, F2, L2);
               Emit_Inst (Ctx.E, D1 & " = sub.i32 " & To_String (L1) & ", " & To_String (F1));
               Emit_Inst (Ctx.E, N1 & " = add.i32 " & D1 & ", 1");
               Emit_Inst (Ctx.E, P1 & " = cmp_gt_s.i32 " & N1 & ", 0");
               Emit_Inst (Ctx.E, C1 & " = select.i32 " & P1 & ", " & N1 & ", 0");
               Emit_Inst (Ctx.E, D2 & " = sub.i32 " & To_String (L2) & ", " & To_String (F2));
               Emit_Inst (Ctx.E, N2 & " = add.i32 " & D2 & ", 1");
               Emit_Inst (Ctx.E, P2 & " = cmp_gt_s.i32 " & N2 & ", 0");
               Emit_Inst (Ctx.E, C2 & " = select.i32 " & P2 & ", " & N2 & ", 0");
               Emit_Inst (Ctx.E, Mlt & " = cmp_lt_s.i32 " & C1 & ", " & C2);
               Emit_Inst (Ctx.E, MinL & " = select.i32 " & Mlt & ", " & C1 & ", " & C2);
               Emit_Inst (Ctx.E, Min64 & " = sext.i64 " & MinL);
               Emit_Inst (Ctx.E, LCmp & " = " & CmpOp & ".i32 " & C1 & ", " & C2);
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
               Emit_Inst (Ctx.E, ECmp & " = " & CmpOp & "." & EP & " " & V1 & ", " & V2);
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
               declare
                  R64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (R_Reg), To_String (R_V.Pir_Type), "f64");
                  Dv  : constant String := Next_Reg (Ctx.E, "fxd");
                  Si  : constant String := Next_Reg (Ctx.E, "fxi");
               begin
                  Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & R64 & ", " & Format_Float64 (Fixed_Small (LT)));
                  Emit_Inst (Ctx.E, Si & " = fptosi." & PT & " " & Dv & " [sat]");
                  R_Reg := To_Unbounded_String (Si);
               end;
            elsif Is_Fixed_Type (R_V.Ada_Type) and then Is_Real then
               declare
                  L64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (L_Reg), PT, "f64");
                  Dv  : constant String := Next_Reg (Ctx.E, "fxd");
                  Si  : constant String := Next_Reg (Ctx.E, "fxi");
               begin
                  Op_PT   := R_V.Pir_Type;
                  Is_Real := False;
                  Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & L64 & ", " & Format_Float64 (Fixed_Small (R_V.Ada_Type)));
                  Emit_Inst (Ctx.E, Si & " = fptosi." & To_String (Op_PT) & " " & Dv & " [sat]");
                  L_Reg := To_Unbounded_String (Si);
               end;
            elsif Op_Pos /= 20 and then To_String (R_V.Pir_Type) /= PT then
               if not Is_Real and then (To_String (R_V.Pir_Type) = "f32" or else To_String (R_V.Pir_Type) = "f64") then
                  Op_PT   := R_V.Pir_Type;
                  Is_Real := True;
                  L_Reg   := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (L_Reg), PT, To_String (Op_PT)));
               else
                  R_Reg := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (R_Reg), To_String (R_V.Pir_Type), PT));
               end if;
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
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_lt_s." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 10 => -- <=
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_ole." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_le_s." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 11 => -- >
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_ogt." & Use_PT & " " & Use_L & ", " & Use_R);
                     else
                        Emit_Inst (Ctx.E, Out_R & " = cmp_gt_s." & Use_PT & " " & Use_L & ", " & Use_R);
                     end if;
                  when 12 => -- >=
                     if Is_Real then
                        Emit_Inst (Ctx.E, Out_R & " = fcmp_oge." & Use_PT & " " & Use_L & ", " & Use_R);
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

   -- Handle predefined RTL calls (Text_IO, Calendar, Unchecked_Deallocation, Unchecked_Conversion)
   function Try_Emit_RTL_Call
     (Ctx        : in out Context;
      Def        : TREE;
      Param_List : SEQ_TYPE;
      Res_Type   : TREE;
      Handled    : out Boolean) return Expr_Val
   is
      Pkg  : constant String := Enclosing_Package_Name (Def);
      Name : constant String := Sym_Name (Def);
      Res  : Expr_Val;
      PL   : SEQ_TYPE := Param_List;
      Args : array (1 .. 8) of TREE := (others => CONST_VOID);
      NArg : Natural := 0;
   begin
      Handled := False;
      while not IS_EMPTY (PL) and then NArg < 8 loop
         NArg := NArg + 1;
         POP (PL, Args (NArg));
      end loop;

      -- Check for generic instantiation of UNCHECKED_DEALLOCATION / UNCHECKED_CONVERSION
      if KIND (Def) in CLASS_SUBPROG_NAME then
         declare
            UD : constant TREE := D (SM_UNIT_DESC, Def);
         begin
            if not Is_Void_Tree (UD) and then KIND (UD) = DN_INSTANTIATION then
               declare
                  Gen_Def : constant TREE := Get_Name_Defn (D (AS_NAME, UD));
                  Gen_N   : constant String := Sym_Name (Gen_Def);
               begin
                  if Gen_N = "UNCHECKED_DEALLOCATION" and then Is_Library_Level (Gen_Def) then
                     Handled := True;
                     declare
                        LV    : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                        Acc_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, LV.Ada_Type));
                        Des_T : constant TREE :=
                          (if not Is_Void_Tree (Acc_T) and then KIND (Acc_T) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                           then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Acc_T))) else CONST_VOID);
                        Pool_Base : TREE := Base_Type_Spec (Acc_T);
                        Has_Pool  : Boolean := False;
                        PV    : constant String := Next_Reg (Ctx.E, "fp");
                        Dummy : Unbounded_String;
                     begin
                        declare
                           Cur : TREE := Pool_Base;
                        begin
                           for Step in 1 .. 16 loop
                              exit when Is_Void_Tree (Cur) or else KIND (Cur) /= DN_ACCESS;
                              if not Is_Void_Tree (D (SM_STORAGE_SIZE, Cur)) then
                                 Pool_Base := Cur;
                                 Has_Pool  := True;
                                 exit;
                              end if;
                              Cur := Resolve_Type_Spec (D (SM_DERIVED, Cur));
                           end loop;
                        end;
                        Emit_Inst (Ctx.E, PV & " = load.ptr " & To_String (LV.Reg) & ", align 8");
                        if not Is_Void_Tree (Des_T) and then Is_Task_Type (Des_T) then
                           -- LRM 13.10.1 / cda101b: task continues executing; only null out the access variable
                           null;
                        elsif Has_Pool then
                           declare
                              PSym : constant String := "@ada_pool_used_" & Tree_Key_Str (Pool_Base);
                              UP   : constant String := Next_Reg (Ctx.E, "pup");
                           begin
                              Add_Global_Decl (Ctx.E, PSym, PSym & " = global i64 = 0, align 8");
                              Emit_Inst (Ctx.E, UP & " = addr " & PSym);
                              if Is_Access_To_Uncon_Array (Acc_T) then
                                 declare
                                    L_Nn : constant String := Next_Label (Ctx.E, "pnn");
                                    L_Dn : constant String := Next_Label (Ctx.E, "pdn");
                                    Cnd  : constant String := Next_Reg (Ctx.E, "pnc");
                                    P0   : constant String := Next_Reg (Ctx.E, "pp0");
                                    DP   : constant String := Next_Reg (Ctx.E, "pdp");
                                 begin
                                    Emit_Inst (Ctx.E, Cnd & " = cmp_ne.ptr " & PV & ", 0");
                                    Emit_Term (Ctx.E, "br " & Cnd & ", " & L_Nn & ", " & L_Dn);
                                    Emit_Label (Ctx.E, L_Nn);
                                    Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & PV & ", 0");
                                    Emit_Inst (Ctx.E, DP & " = load.ptr " & P0 & ", align 8");
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_pool_free", UP & ", " & DP, "", Dummy, Nounwind => True);
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_free", PV, "", Dummy, Nounwind => True);
                                    Emit_Term (Ctx.E, "jmp " & L_Dn);
                                    Emit_Label (Ctx.E, L_Dn);
                                 end;
                              else
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_pool_free", UP & ", " & PV, "", Dummy, Nounwind => True);
                              end if;
                           end;
                        elsif Is_Access_To_Uncon_Array (Acc_T) then
                           declare
                              L_Nn : constant String := Next_Label (Ctx.E, "unn");
                              L_Dn : constant String := Next_Label (Ctx.E, "udn");
                              Cnd  : constant String := Next_Reg (Ctx.E, "unc");
                              P0   : constant String := Next_Reg (Ctx.E, "up0");
                              DP   : constant String := Next_Reg (Ctx.E, "udp");
                           begin
                              Emit_Inst (Ctx.E, Cnd & " = cmp_ne.ptr " & PV & ", 0");
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_Nn & ", " & L_Dn);
                              Emit_Label (Ctx.E, L_Nn);
                              Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & PV & ", 0");
                              Emit_Inst (Ctx.E, DP & " = load.ptr " & P0 & ", align 8");
                              Emit_Call_Or_Invoke (Ctx, "@__ada_free", DP, "", Dummy, Nounwind => True);
                              Emit_Call_Or_Invoke (Ctx, "@__ada_free", PV, "", Dummy, Nounwind => True);
                              Emit_Term (Ctx.E, "jmp " & L_Dn);
                              Emit_Label (Ctx.E, L_Dn);
                           end;
                        else
                           Emit_Call_Or_Invoke (Ctx, "@__ada_free", PV, "", Dummy, Nounwind => True);
                        end if;
                        Emit_Inst (Ctx.E, "store.ptr 0, " & To_String (LV.Reg) & ", align 8");
                     end;
                     return Res;
                  elsif Gen_N = "UNCHECKED_CONVERSION" and then Is_Library_Level (Gen_Def) then
                     Handled := True;
                     declare
                        Src : constant Expr_Val := Emit_Expr (Ctx, Args (1));
                        STI : constant Type_Info := Get_Type_Info (Ctx, Src.Ada_Type);
                        RTI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
                        R   : constant String := Next_Reg (Ctx.E, "uc");

                        function Is_Bool_Bit_Array (ATi : Type_Info) return Boolean is
                        begin
                           if (ATi.Kind = TK_Constrained_Array or else ATi.Kind = TK_Unconstrained_Array)
                             and then ATi.NDims = 1
                             and then not Is_Void_Tree (ATi.Elem_Type)
                             and then KIND (Base_Type_Spec (ATi.Elem_Type)) = DN_ENUMERATION
                             and then Sym_Name (D (XD_SOURCE_NAME, Base_Type_Spec (ATi.Elem_Type))) = "BOOLEAN"
                           then
                              return True;
                           end if;
                           return False;
                        end Is_Bool_Bit_Array;
                     begin
                        if RTI.Kind /= TK_Scalar and then STI.Kind = TK_Scalar
                          and then Is_Bool_Bit_Array (RTI)
                        then
                           -- cda201a/b, cd2a31c (LENGTH_CHECK): unpack scalar bits into 1D BOOLEAN array (static or dynamic!)
                           declare
                              F_R, L_R : Unbounded_String;
                              NB64 : Unbounded_String;
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              IV  : Unbounded_String;
                              L_H : constant String := Next_Label (Ctx.E, "ubu_h");
                              L_B : constant String := Next_Label (Ctx.E, "ubu_b");
                              L_E : constant String := Next_Label (Ctx.E, "ubu_e");
                              IR  : constant String := Next_Reg (Ctx.E, "ubi");
                              Cnd : constant String := Next_Reg (Ctx.E, "ubc");
                              Lt64 : constant String := Next_Reg (Ctx.E, "ubl6");
                              Sh  : constant String := Next_Reg (Ctx.E, "ubs");
                              Bt  : constant String := Next_Reg (Ctx.E, "ubt");
                              Sel : constant String := Next_Reg (Ctx.E, "ubz");
                              B8  : constant String := Next_Reg (Ctx.E, "ub8");
                              EP  : constant String := Next_Reg (Ctx.E, "ubp");
                              Nx  : constant String := Next_Reg (Ctx.E, "ubn");
                           begin
                              if To_String (Src.Pir_Type) = "f32" then
                                 declare
                                    B32 : constant String := Next_Reg (Ctx.E, "ubf");
                                 begin
                                    Emit_Inst (Ctx.E, B32 & " = bitcast.i32 " & To_String (Src.Reg));
                                    IV := To_Unbounded_String (Emit_Int_Convert (Ctx, B32, "i32", "i64"));
                                 end;
                              elsif To_String (Src.Pir_Type) = "f64" then
                                 declare
                                    B64 : constant String := Next_Reg (Ctx.E, "ubf");
                                 begin
                                    Emit_Inst (Ctx.E, B64 & " = bitcast.i64 " & To_String (Src.Reg));
                                    IV := To_Unbounded_String (B64);
                                 end;
                              else
                                 IV := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Src.Reg), To_String (Src.Pir_Type), "i64"));
                                 -- Map enumeration POS to SM_REP code if an enumeration representation clause applies (cd3014*, cd3015*, cd3022a, cd1009*)!
                                 declare
                                    Cur_E : TREE := Resolve_Type_Spec (Base_Type_Spec (Subst_Tree (Ctx, Src.Ada_Type)));
                                    Rep_Found : Boolean := False;
                                    Tab_Init  : Unbounded_String;
                                 begin
                                    while not Is_Void_Tree (Cur_E) and then KIND (Cur_E) = DN_ENUMERATION loop
                                       declare
                                          LS : constant TREE := D (SM_LITERAL_S, Cur_E);
                                       begin
                                          if not Is_Void_Tree (LS) then
                                             declare
                                                LL  : SEQ_TYPE := LIST (LS);
                                                Lit : TREE;
                                                Has_Custom : Boolean := False;
                                                Buf_T : Unbounded_String;
                                                Cnt   : Natural := 0;
                                             begin
                                                while not IS_EMPTY (LL) loop
                                                   POP (LL, Lit);
                                                   declare
                                                      Pos_V : constant Integer_64 := Integer_64 (DI (SM_POS, Lit));
                                                      Rep_N : constant TREE := D (SM_REP, Lit);
                                                      Rep_V : constant Integer_64 :=
                                                        (if Has_Static_Val (Rep_N) then Eval_Int64 (Rep_N) else Pos_V);
                                                   begin
                                                      if Rep_V /= Pos_V then
                                                         Has_Custom := True;
                                                      end if;
                                                      declare
                                                         UV : Unsigned_64 := (if Rep_V >= 0 then Unsigned_64 (Rep_V) else Unsigned_64'Mod (Rep_V));
                                                      begin
                                                         for B in 0 .. 7 loop
                                                            if Cnt > 0 or else B > 0 then Append (Buf_T, ", "); end if;
                                                            Append (Buf_T, Img (Natural (UV and 16#FF#)));
                                                            UV := Shift_Right (UV, 8);
                                                         end loop;
                                                      end;
                                                      Cnt := Cnt + 1;
                                                   end;
                                                end loop;
                                                if Has_Custom and then Cnt > 0 then
                                                   Tab_Init := Buf_T;
                                                   Rep_Found := True;
                                                   exit;
                                                end if;
                                             end;
                                          end if;
                                       end;
                                       Cur_E := Resolve_Type_Spec (D (SM_DERIVED, Cur_E));
                                    end loop;
                                    if Rep_Found then
                                       declare
                                          TSym : constant String := "@ada_erep_" & Tree_Key_Str (Cur_E);
                                          TA   : constant String := Next_Reg (Ctx.E, "era");
                                          EP   : constant String := Next_Reg (Ctx.E, "erep");
                                          RV   : constant String := Next_Reg (Ctx.E, "erv");
                                       begin
                                          Add_Data_Decl (Ctx.E, TSym,
                                            TSym & " = data align 8 { " & To_String (Tab_Init) & " }");
                                          Emit_Inst (Ctx.E, TA & " = addr " & TSym);
                                          Emit_Inst (Ctx.E, EP & " = elem_ptr i64, " & TA & ", " & To_String (IV));
                                          Emit_Inst (Ctx.E, RV & " = load.i64 " & EP & ", align 8");
                                          IV := To_Unbounded_String (RV);
                                       end;
                                    end if;
                                 end;
                              end if;
                              if RTI.Kind = TK_Constrained_Array and then not RTI.Is_Dynamic and then RTI.Dims (1).Is_Static then
                                 declare
                                    Len : constant Integer_64 := (if RTI.Dims (1).Last >= RTI.Dims (1).First then RTI.Dims (1).Last - RTI.Dims (1).First + 1 else 0);
                                    NR  : constant String := Next_Reg (Ctx.E, "ubn0");
                                 begin
                                    Emit_Inst (Ctx.E, NR & " = const.i64 " & Img (Long_Long_Integer (Len)));
                                    NB64 := To_Unbounded_String (NR);
                                    Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, Natural (Len))) & " x i8], align 1");
                                 end;
                              else
                                 Emit_Index_Bounds (Ctx, RTI.Dims (1).Idx_Type, F_R, L_R);
                                 NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_R), To_String (L_R)));
                                 declare
                                    Nz  : constant String := Next_Reg (Ctx.E, "ubnz");
                                    AlB : constant String := Next_Reg (Ctx.E, "ubal");
                                 begin
                                    Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (NB64) & ", 0");
                                    Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (NB64) & ", 1");
                                    Emit_Inst (Ctx.E, Buf & " = alloca_dyn " & AlB & ", align 1");
                                 end;
                              end if;
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (NB64));
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, Lt64 & " = cmp_lt_s.i64 " & IR & ", 64");
                              Emit_Inst (Ctx.E, Sh & " = shr.i64 " & To_String (IV) & ", " & IR);
                              Emit_Inst (Ctx.E, Bt & " = and.i64 " & Sh & ", 1");
                              Emit_Inst (Ctx.E, Sel & " = select.i64 " & Lt64 & ", " & Bt & ", 0");
                              Emit_Inst (Ctx.E, B8 & " = trunc.i8 " & Sel);
                              Emit_Inst (Ctx.E, EP & " = elem_ptr i8, " & Buf & ", " & IR);
                              Emit_Inst (Ctx.E, "store.i8 " & B8 & ", " & EP & ", align 1");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              if RTI.Kind = TK_Constrained_Array and then not RTI.Is_Dynamic then
                                 Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := 1;
                              else
                                 Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (Buf);
                                 Res.Pir_Type := To_Unbounded_String ("ptr"); Res.Ada_Type := Res_Type; Res.Align := 1;
                                 Res.NDims := 1; Res.First_Regs (1) := F_R; Res.Last_Regs (1) := L_R;
                              end if;
                           end;
                        elsif RTI.Kind = TK_Scalar and then STI.Kind /= TK_Scalar
                          and then Is_Bool_Bit_Array (STI)
                        then
                           -- cda201a/b, cd2a31c: pack 1D BOOLEAN array (static or dynamic) into scalar (with sign-extension for signed ints)
                           declare
                              SF, SL : Unbounded_String;
                              NB64   : Unbounded_String;
                              Lim64  : constant String := Next_Reg (Ctx.E, "upl");
                              Lt64   : constant String := Next_Reg (Ctx.E, "uplc");
                              Acc : constant String := Next_Reg (Ctx.E, "upa");
                              L_H : constant String := Next_Label (Ctx.E, "ubp_h");
                              L_B : constant String := Next_Label (Ctx.E, "ubp_b");
                              L_E : constant String := Next_Label (Ctx.E, "ubp_e");
                              IR  : constant String := Next_Reg (Ctx.E, "upi");
                              Cnd : constant String := Next_Reg (Ctx.E, "upc");
                              EP  : constant String := Next_Reg (Ctx.E, "upp");
                              EV  : constant String := Next_Reg (Ctx.E, "upv");
                              E64 : constant String := Next_Reg (Ctx.E, "up6");
                              E1  : constant String := Next_Reg (Ctx.E, "up1");
                              Sh  : constant String := Next_Reg (Ctx.E, "ups");
                              Cur : constant String := Next_Reg (Ctx.E, "upk");
                              Nw  : constant String := Next_Reg (Ctx.E, "upw");
                              Nx  : constant String := Next_Reg (Ctx.E, "upn");
                              Fin : Unbounded_String := To_Unbounded_String (Next_Reg (Ctx.E, "upf"));
                           begin
                              Get_Array_Dim_Bounds (Ctx, Src, 1, SF, SL);
                              NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (SF), To_String (SL)));
                              Emit_Inst (Ctx.E, Lt64 & " = cmp_lt_s.i64 " & To_String (NB64) & ", 64");
                              Emit_Inst (Ctx.E, Lim64 & " = select.i64 " & Lt64 & ", " & To_String (NB64) & ", 64");
                              Emit_Entry_Alloca (Ctx.E, Acc & " = alloca i64, align 8");
                              Emit_Inst (Ctx.E, "store.i64 0, " & Acc & ", align 8");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Lim64);
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, EP & " = elem_ptr i8, " & To_String (Src.Reg) & ", " & IR);
                              Emit_Inst (Ctx.E, EV & " = load.i8 " & EP & ", align 1");
                              Emit_Inst (Ctx.E, E64 & " = zext.i64 " & EV);
                              Emit_Inst (Ctx.E, E1 & " = and.i64 " & E64 & ", 1");
                              Emit_Inst (Ctx.E, Sh & " = shl.i64 " & E1 & ", " & IR);
                              Emit_Inst (Ctx.E, Cur & " = load.i64 " & Acc & ", align 8");
                              Emit_Inst (Ctx.E, Nw & " = or.i64 " & Cur & ", " & Sh);
                              Emit_Inst (Ctx.E, "store.i64 " & Nw & ", " & Acc & ", align 8");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              Emit_Inst (Ctx.E, To_String (Fin) & " = load.i64 " & Acc & ", align 8");
                              -- Sign-extend narrow bit-width into signed integer types (e.g. 9-bit -60 in LENGTH_CHECK)
                              if Is_Integer_Type (Res_Type) then
                                 declare
                                    St_R : Boolean;
                                    RLo, RHi : Integer_64;
                                 begin
                                    Get_Discrete_Bounds (Resolve_Type_Spec (Res_Type), St_R, RLo, RHi);
                                    if not St_R or else RLo < 0 then
                                       declare
                                          Need_S : constant String := Next_Reg (Ctx.E, "upsn");
                                          ShAmt  : constant String := Next_Reg (Ctx.E, "upsa");
                                          ShL    : constant String := Next_Reg (Ctx.E, "upsl");
                                          ShR    : constant String := Next_Reg (Ctx.E, "upsr");
                                          SFin   : constant String := Next_Reg (Ctx.E, "upsf");
                                          Gt0    : constant String := Next_Reg (Ctx.E, "upg0");
                                          Lt6    : constant String := Next_Reg (Ctx.E, "upl6");
                                          SafeA  : constant String := Next_Reg (Ctx.E, "upsz");
                                       begin
                                          Emit_Inst (Ctx.E, Gt0 & " = cmp_gt_s.i64 " & Lim64 & ", 0");
                                          Emit_Inst (Ctx.E, Lt6 & " = cmp_lt_s.i64 " & Lim64 & ", 64");
                                          Emit_Inst (Ctx.E, Need_S & " = and.i8 " & Gt0 & ", " & Lt6);
                                          Emit_Inst (Ctx.E, ShAmt & " = sub.i64 64, " & Lim64);
                                          Emit_Inst (Ctx.E, SafeA & " = select.i64 " & Need_S & ", " & ShAmt & ", 0");
                                          Emit_Inst (Ctx.E, ShL & " = shl.i64 " & To_String (Fin) & ", " & SafeA);
                                          Emit_Inst (Ctx.E, ShR & " = sar.i64 " & ShL & ", " & SafeA);
                                          Emit_Inst (Ctx.E, SFin & " = select.i64 " & Need_S & ", " & ShR & ", " & To_String (Fin));
                                          Fin := To_Unbounded_String (SFin);
                                       end;
                                    end if;
                                 end;
                              end if;
                              -- LRM 13.4 (cd2a23a/b, cd2a24a): reverse-map custom enum SM_REP -> SM_POS when unpacking FROM_BITS!
                              declare
                                 B_Res : constant TREE := Subst_Base_Type (Ctx, Res_Type);
                              begin
                                 if not Is_Void_Tree (B_Res) and then KIND (B_Res) = DN_ENUMERATION
                                   and then not Is_Void_Tree (D (SM_LITERAL_S, B_Res))
                                 then
                                    declare
                                       Lits : SEQ_TYPE := LIST (D (SM_LITERAL_S, B_Res));
                                       Lit  : TREE;
                                       Has_Custom : Boolean := False;
                                       Pos_V, Rep_V : Integer_64;
                                    begin
                                       while not IS_EMPTY (Lits) loop
                                          POP (Lits, Lit);
                                          if KIND (Lit) in DN_ENUMERATION_ID | DN_CHARACTER_ID then
                                             Pos_V := Eval_Int64 (D (SM_POS, Lit));
                                             Rep_V := (if not Is_Void_Tree (D (SM_REP, Lit)) then Eval_Int64 (D (SM_REP, Lit)) else Pos_V);
                                             if Rep_V /= Pos_V then Has_Custom := True; exit; end if;
                                          end if;
                                       end loop;
                                       if Has_Custom then
                                          declare
                                             Cur_Pos : Unbounded_String := Fin;
                                          begin
                                             Lits := LIST (D (SM_LITERAL_S, B_Res));
                                             while not IS_EMPTY (Lits) loop
                                                POP (Lits, Lit);
                                                if KIND (Lit) in DN_ENUMERATION_ID | DN_CHARACTER_ID then
                                                   Pos_V := Eval_Int64 (D (SM_POS, Lit));
                                                   Rep_V := (if not Is_Void_Tree (D (SM_REP, Lit)) then Eval_Int64 (D (SM_REP, Lit)) else Pos_V);
                                                   declare
                                                      Eq  : constant String := Next_Reg (Ctx.E, "ereq");
                                                      Sel : constant String := Next_Reg (Ctx.E, "ersl");
                                                   begin
                                                      Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & To_String (Fin) & ", " & Img (Long_Long_Integer (Rep_V)));
                                                      Emit_Inst (Ctx.E, Sel & " = select.i64 " & Eq & ", " & Img (Long_Long_Integer (Pos_V)) & ", " & To_String (Cur_Pos));
                                                      Cur_Pos := To_Unbounded_String (Sel);
                                                   end;
                                                end if;
                                             end loop;
                                             Fin := Cur_Pos;
                                          end;
                                       end if;
                                    end;
                                 end if;
                              end;
                              if To_String (RTI.Pir_Type) = "f32" then
                                 declare
                                    T32 : constant String := Next_Reg (Ctx.E, "up3");
                                 begin
                                    Emit_Inst (Ctx.E, T32 & " = trunc.i32 " & To_String (Fin));
                                    Emit_Inst (Ctx.E, R & " = bitcast.f32 " & T32);
                                 end;
                              elsif To_String (RTI.Pir_Type) = "f64" then
                                 Emit_Inst (Ctx.E, R & " = bitcast.f64 " & To_String (Fin));
                              else
                                 declare
                                    Cv : constant String := Emit_Int_Convert (Ctx, To_String (Fin), "i64", To_String (RTI.Pir_Type));
                                 begin
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (Cv);
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                                    return Res;
                                 end;
                              end if;
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                           end;
                        elsif Is_Bool_Bit_Array (RTI) and then STI.Kind /= TK_Scalar then
                           -- Composite -> Bool bit-array (cd4061a): unpack source bytes bit-by-bit into 0/1 BOOLEANs
                           declare
                              F_R, L_R : Unbounded_String;
                              NB64     : Unbounded_String;
                              AlB      : constant String := Next_Reg (Ctx.E, "ucab");
                              Nz       : constant String := Next_Reg (Ctx.E, "ucnz");
                              Buf      : constant String := Next_Reg (Ctx.E, "ucbf");
                              L_H      : constant String := Next_Label (Ctx.E, "ucb_h");
                              L_B      : constant String := Next_Label (Ctx.E, "ucb_b");
                              L_E      : constant String := Next_Label (Ctx.E, "ucb_e");
                              IR       : constant String := Next_Reg (Ctx.E, "ucbi");
                              Cnd      : constant String := Next_Reg (Ctx.E, "ucbc");
                              ByI      : constant String := Next_Reg (Ctx.E, "ucby");
                              BiI      : constant String := Next_Reg (Ctx.E, "ucbt");
                              InB      : constant String := Next_Reg (Ctx.E, "ucin");
                              SB_P     : constant String := Next_Reg (Ctx.E, "ucsp");
                              SB_V     : constant String := Next_Reg (Ctx.E, "ucsv");
                              S64      : constant String := Next_Reg (Ctx.E, "ucs6");
                              Sh       : constant String := Next_Reg (Ctx.E, "ucsh");
                              B1       : constant String := Next_Reg (Ctx.E, "ucb1");
                              B8       : constant String := Next_Reg (Ctx.E, "ucb8");
                              BV       : constant String := Next_Reg (Ctx.E, "ucbv");
                              DP       : constant String := Next_Reg (Ctx.E, "ucdp");
                              Nx       : constant String := Next_Reg (Ctx.E, "ucnx");
                              SSz      : constant Natural := Natural'Max (1, STI.Byte_Size);
                           begin
                              Emit_Index_Bounds (Ctx, RTI.Dims (1).Idx_Type, F_R, L_R);
                              NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_R), To_String (L_R)));
                              Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (NB64) & ", 0");
                              Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (NB64) & ", 1");
                              Emit_Inst (Ctx.E, Buf & " = alloca_dyn " & AlB & ", align 1");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (NB64));
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, ByI & " = shr.i64 " & IR & ", 3");
                              Emit_Inst (Ctx.E, BiI & " = and.i64 " & IR & ", 7");
                              Emit_Inst (Ctx.E, InB & " = cmp_lt_s.i64 " & ByI & ", " & Img (SSz));
                              declare
                                 SafeI : constant String := Next_Reg (Ctx.E, "ucsi");
                              begin
                                 Emit_Inst (Ctx.E, SafeI & " = select.i64 " & InB & ", " & ByI & ", 0");
                                 Emit_Inst (Ctx.E, SB_P & " = elem_ptr i8, " & To_String (Src.Reg) & ", " & SafeI);
                              end;
                              Emit_Inst (Ctx.E, SB_V & " = load.i8 " & SB_P & ", align 1");
                              Emit_Inst (Ctx.E, S64 & " = zext.i64 " & SB_V);
                              Emit_Inst (Ctx.E, Sh & " = shr.i64 " & S64 & ", " & BiI);
                              Emit_Inst (Ctx.E, B1 & " = and.i64 " & Sh & ", 1");
                              Emit_Inst (Ctx.E, B8 & " = trunc.i8 " & B1);
                              Emit_Inst (Ctx.E, BV & " = select.i8 " & InB & ", " & B8 & ", 0");
                              Emit_Inst (Ctx.E, DP & " = elem_ptr i8, " & Buf & ", " & IR);
                              Emit_Inst (Ctx.E, "store.i8 " & BV & ", " & DP & ", align 1");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := To_Unbounded_String ("ptr"); Res.Ada_Type := Res_Type;
                              Res.Align := 1; Res.NDims := 1;
                              Res.First_Regs (1) := F_R; Res.Last_Regs (1) := L_R;
                           end;
                        elsif Is_Bool_Bit_Array (STI) and then RTI.Kind /= TK_Scalar then
                           -- Bool bit-array -> Composite (cd4061a): pack 0/1 BOOLEANs bit-by-bit into target bytes
                           declare
                              SF, SL : Unbounded_String;
                              NB64   : Unbounded_String;
                              DSz    : constant Natural := Natural'Max (1, RTI.Byte_Size);
                              DAl    : constant Natural := Natural'Max (1, RTI.Align);
                              MaxB   : constant Natural := DSz * 8;
                              LtM    : constant String := Next_Reg (Ctx.E, "uplm");
                              Lim    : constant String := Next_Reg (Ctx.E, "upli");
                              Buf    : constant String := Next_Reg (Ctx.E, "upbf");
                              L_H    : constant String := Next_Label (Ctx.E, "upb_h");
                              L_B    : constant String := Next_Label (Ctx.E, "upb_b");
                              L_E    : constant String := Next_Label (Ctx.E, "upb_e");
                              IR     : constant String := Next_Reg (Ctx.E, "upbi");
                              Cnd    : constant String := Next_Reg (Ctx.E, "upbc");
                              SP     : constant String := Next_Reg (Ctx.E, "upsp");
                              SV     : constant String := Next_Reg (Ctx.E, "upsv");
                              B1     : constant String := Next_Reg (Ctx.E, "upb1");
                              ByI    : constant String := Next_Reg (Ctx.E, "upby");
                              BiI    : constant String := Next_Reg (Ctx.E, "upbt");
                              Bi8    : constant String := Next_Reg (Ctx.E, "up8");
                              Sh     : constant String := Next_Reg (Ctx.E, "upsh");
                              DP     : constant String := Next_Reg (Ctx.E, "updp");
                              Cur    : constant String := Next_Reg (Ctx.E, "upcr");
                              Nw     : constant String := Next_Reg (Ctx.E, "upnw");
                              Nx     : constant String := Next_Reg (Ctx.E, "upnx");
                           begin
                              Get_Array_Dim_Bounds (Ctx, Src, 1, SF, SL);
                              NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (SF), To_String (SL)));
                              Emit_Inst (Ctx.E, LtM & " = cmp_lt_s.i64 " & To_String (NB64) & ", " & Img (MaxB));
                              Emit_Inst (Ctx.E, Lim & " = select.i64 " & LtM & ", " & To_String (NB64) & ", " & Img (MaxB));
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (DSz) & " x i8], align " & Img (DAl));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (DSz) & ", align " & Img (DAl));
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Lim);
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, SP & " = elem_ptr i8, " & To_String (Src.Reg) & ", " & IR);
                              Emit_Inst (Ctx.E, SV & " = load.i8 " & SP & ", align 1");
                              Emit_Inst (Ctx.E, B1 & " = and.i8 " & SV & ", 1");
                              Emit_Inst (Ctx.E, ByI & " = shr.i64 " & IR & ", 3");
                              Emit_Inst (Ctx.E, BiI & " = and.i64 " & IR & ", 7");
                              Emit_Inst (Ctx.E, Bi8 & " = trunc.i8 " & BiI);
                              Emit_Inst (Ctx.E, Sh & " = shl.i8 " & B1 & ", " & Bi8);
                              Emit_Inst (Ctx.E, DP & " = elem_ptr i8, " & Buf & ", " & ByI);
                              Emit_Inst (Ctx.E, Cur & " = load.i8 " & DP & ", align 1");
                              Emit_Inst (Ctx.E, Nw & " = or.i8 " & Cur & ", " & Sh);
                              Emit_Inst (Ctx.E, "store.i8 " & Nw & ", " & DP & ", align 1");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              -- Sign-extend narrow signed integer single-component record (cd4061a: 5-bit -7 -> i8 -7)
                              if RTI.Kind = TK_Record and then DSz <= 8 then
                                 declare
                                    BR : constant TREE := Subst_Base_Type (Ctx, Res_Type);
                                    CL : constant TREE := (if not Is_Void_Tree (BR) and then KIND (BR) = DN_RECORD then D (SM_COMP_LIST, BR) else CONST_VOID);
                                 begin
                                    if not Is_Void_Tree (CL) and then not Is_Void_Tree (D (AS_DECL_S, CL)) then
                                       declare
                                          DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                                          C1 : TREE;
                                       begin
                                          if not IS_EMPTY (DL) then
                                             POP (DL, C1);
                                             if IS_EMPTY (DL) and then KIND (C1) = DN_VARIABLE_DECL then
                                                declare
                                                   NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, C1));
                                                   CID : TREE;
                                                begin
                                                   if not IS_EMPTY (NS) then
                                                      POP (NS, CID);
                                                      if IS_EMPTY (NS) then
                                                         declare
                                                            CT  : constant TREE := Subst_Resolve_Type (Ctx, D (SM_OBJ_TYPE, CID));
                                                            CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                                            St_C : Boolean := False;
                                                            CLo, CHi : Integer_64 := 0;
                                                         begin
                                                            if Is_Integer_Type (CT) then
                                                               Get_Discrete_Bounds (CT, St_C, CLo, CHi);
                                                               if not St_C or else CLo < 0 then
                                                                  declare
                                                                     FBits  : constant Natural := CTI.Byte_Size * 8;
                                                                     Lv0    : constant String := Next_Reg (Ctx.E, "rslv");
                                                                     Lv64   : Unbounded_String;
                                                                     Gt0    : constant String := Next_Reg (Ctx.E, "rsg0");
                                                                     LtF    : constant String := Next_Reg (Ctx.E, "rslf");
                                                                     NeedS  : constant String := Next_Reg (Ctx.E, "rsns");
                                                                     ShA    : constant String := Next_Reg (Ctx.E, "rssa");
                                                                     SafeA  : constant String := Next_Reg (Ctx.E, "rsza");
                                                                     ShL    : constant String := Next_Reg (Ctx.E, "rssl");
                                                                     ShR    : constant String := Next_Reg (Ctx.E, "rssr");
                                                                     S64    : constant String := Next_Reg (Ctx.E, "rss6");
                                                                     OutV   : Unbounded_String;
                                                                  begin
                                                                     Emit_Inst (Ctx.E, Lv0 & " = load." & To_String (CTI.Pir_Type) & " " & Buf & ", align " & Img (DAl));
                                                                     Lv64 := To_Unbounded_String (Emit_Int_Convert (Ctx, Lv0, To_String (CTI.Pir_Type), "i64"));
                                                                     Emit_Inst (Ctx.E, Gt0 & " = cmp_gt_s.i64 " & Lim & ", 0");
                                                                     Emit_Inst (Ctx.E, LtF & " = cmp_lt_s.i64 " & Lim & ", " & Img (FBits));
                                                                     Emit_Inst (Ctx.E, NeedS & " = and.i8 " & Gt0 & ", " & LtF);
                                                                     Emit_Inst (Ctx.E, ShA & " = sub.i64 64, " & Lim);
                                                                     Emit_Inst (Ctx.E, SafeA & " = select.i64 " & NeedS & ", " & ShA & ", 0");
                                                                     Emit_Inst (Ctx.E, ShL & " = shl.i64 " & To_String (Lv64) & ", " & SafeA);
                                                                     Emit_Inst (Ctx.E, ShR & " = sar.i64 " & ShL & ", " & SafeA);
                                                                     Emit_Inst (Ctx.E, S64 & " = select.i64 " & NeedS & ", " & ShR & ", " & To_String (Lv64));
                                                                     OutV := To_Unbounded_String (Emit_Int_Convert (Ctx, S64, "i64", To_String (CTI.Pir_Type)));
                                                                     Emit_Inst (Ctx.E, "store." & To_String (CTI.Pir_Type) & " " & To_String (OutV) & ", " & Buf & ", align " & Img (DAl));
                                                                  end;
                                                               end if;
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
                              Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := DAl;
                           end;
                        elsif RTI.Kind /= TK_Scalar and then STI.Kind /= TK_Scalar then
                           -- Composite -> Composite: allocate target buffer and copy min(src_sz, dst_sz) bytes
                           declare
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              Sz  : constant Natural := Natural'Max (1, Natural'Min (STI.Byte_Size, RTI.Byte_Size));
                              DSz : constant Natural := Natural'Max (1, RTI.Byte_Size);
                              Al  : constant Natural := Natural'Max (1, RTI.Align);
                           begin
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (DSz) & " x i8], align " & Img (Al));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (DSz) & ", align " & Img (Al));
                              Emit_Inst (Ctx.E, "memcpy " & Buf & ", " & To_String (Src.Reg) & ", " & Img (Sz) & ", align 1");
                              Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Al;
                           end;
                        elsif RTI.Kind /= TK_Scalar and then STI.Kind = TK_Scalar then
                           -- Scalar -> Composite: store scalar into target buffer
                           declare
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              DSz : constant Natural := Natural'Max (STI.Byte_Size, RTI.Byte_Size);
                              Al  : constant Natural := Natural'Max (STI.Align, RTI.Align);
                           begin
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, DSz)) & " x i8], align " & Img (Natural'Max (1, Al)));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (Natural'Max (1, DSz)) & ", align 1");
                              Emit_Inst (Ctx.E, "store." & To_String (Src.Pir_Type) & " " & To_String (Src.Reg) & ", " & Buf & ", align " & Img (Natural'Max (1, STI.Align)));
                              Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Natural'Max (1, RTI.Align);
                           end;
                        elsif RTI.Kind = TK_Scalar and then STI.Kind /= TK_Scalar then
                           -- Composite -> Scalar: copy bytes into aligned temp buffer and load scalar
                           declare
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              Sz  : constant Natural := Natural'Max (1, Natural'Min (STI.Byte_Size, RTI.Byte_Size));
                              RSz : constant Natural := Natural'Max (1, RTI.Byte_Size);
                              Al  : constant Natural := Natural'Max (1, RTI.Align);
                           begin
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (RSz) & " x i8], align " & Img (Al));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (RSz) & ", align " & Img (Al));
                              Emit_Inst (Ctx.E, "memcpy " & Buf & ", " & To_String (Src.Reg) & ", " & Img (Sz) & ", align 1");
                              Emit_Inst (Ctx.E, R & " = load." & To_String (RTI.Pir_Type) & " " & Buf & ", align " & Img (Al));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Al;
                           end;
                        elsif To_String (Src.Pir_Type) = To_String (RTI.Pir_Type)
                          or else (STI.Kind = TK_Scalar and then RTI.Kind = TK_Scalar)
                        then
                           -- LRM 13.3 + 13.9: map enumeration SM_REP codes in scalar Unchecked_Conversion
                           declare
                              Src_BT  : constant TREE := Subst_Base_Type (Ctx, Src.Ada_Type);
                              Res_BT  : constant TREE := Subst_Base_Type (Ctx, Res_Type);
                              Eff_Src : Expr_Val := Src;
                              Eff_STI : Type_Info := STI;

                              function Enum_Has_Rep (BT : TREE) return Boolean is
                                 LS  : TREE;
                                 EL  : SEQ_TYPE;
                                 Lit : TREE;
                              begin
                                 if Is_Void_Tree (BT) or else KIND (BT) /= DN_ENUMERATION then
                                    return False;
                                 end if;
                                 LS := D (SM_LITERAL_S, BT);
                                 if Is_Void_Tree (LS) then return False; end if;
                                 EL := LIST (LS);
                                 while not IS_EMPTY (EL) loop
                                    POP (EL, Lit);
                                    if Has_Static_Val (D (SM_REP, Lit))
                                      and then Eval_Int64 (D (SM_REP, Lit)) /= Integer_64 (DI (SM_POS, Lit))
                                    then
                                       return True;
                                    end if;
                                 end loop;
                                 return False;
                              end Enum_Has_Rep;

                              function Emit_Enum_Rep_Table (BT : TREE; Count : out Natural) return String is
                                 Sym : constant String := "@ada_erep_" & Tree_Key_Str (BT);
                                 EL  : SEQ_TYPE := LIST (D (SM_LITERAL_S, BT));
                                 Lit : TREE;
                                 Buf : Unbounded_String;
                                 N   : Natural := 0;
                              begin
                                 while not IS_EMPTY (EL) loop
                                    POP (EL, Lit);
                                    declare
                                       RV : constant TREE := D (SM_REP, Lit);
                                       IV : constant Integer_64 :=
                                         (if Has_Static_Val (RV) then Eval_Int64 (RV)
                                          else Integer_64 (DI (SM_POS, Lit)));
                                       UV : Unsigned_64 :=
                                         (if IV >= 0 then Unsigned_64 (IV)
                                          else Unsigned_64'Mod (IV));
                                    begin
                                       for Bk in 0 .. 7 loop
                                          if N > 0 or else Bk > 0 then Append (Buf, ", "); end if;
                                          Append (Buf, Img (Natural (UV and 16#FF#)));
                                          UV := Shift_Right (UV, 8);
                                       end loop;
                                       N := N + 1;
                                    end;
                                 end loop;
                                 Count := N;
                                 Add_Data_Decl (Ctx.E, Sym, Sym & " = data align 8 { " & To_String (Buf) & " }");
                                 return Sym;
                              end Emit_Enum_Rep_Table;

                              Src_Has_Rep : constant Boolean := Enum_Has_Rep (Src_BT);
                              Res_Has_Rep : constant Boolean := Enum_Has_Rep (Res_BT);
                           begin
                              if Src_Has_Rep then
                                 declare
                                    Cnt : Natural := 0;
                                    Tbl : constant String := Emit_Enum_Rep_Table (Src_BT, Cnt);
                                    P64 : constant String := Emit_Int_Convert (Ctx, To_String (Src.Reg), To_String (Src.Pir_Type), "i64");
                                    TA  : constant String := Next_Reg (Ctx.E, "erpt");
                                    Off : constant String := Next_Reg (Ctx.E, "erpo");
                                    EP  : constant String := Next_Reg (Ctx.E, "erpp");
                                    RV  : constant String := Next_Reg (Ctx.E, "erpv");
                                 begin
                                    pragma Unreferenced (Cnt);
                                    Emit_Inst (Ctx.E, TA & " = addr " & Tbl);
                                    Emit_Inst (Ctx.E, Off & " = mul.i64 " & P64 & ", 8 [unsafe_s]");
                                    Emit_Inst (Ctx.E, EP & " = ptr_add.i64 " & TA & ", " & Off & " [inbounds]");
                                    Emit_Inst (Ctx.E, RV & " = load.i64 " & EP & ", align 8");
                                    Eff_Src.Reg       := To_Unbounded_String (RV);
                                    Eff_Src.Pir_Type  := To_Unbounded_String ("i64");
                                    Eff_STI.Pir_Type  := To_Unbounded_String ("i64");
                                    Eff_STI.Byte_Size := 8;
                                    Eff_STI.Align     := 8;
                                 end;
                              end if;

                              if Res_Has_Rep then
                                 declare
                                    Cnt  : Natural := 0;
                                    Tbl  : constant String := Emit_Enum_Rep_Table (Res_BT, Cnt);
                                    V64  : constant String :=
                                      (if To_String (Eff_Src.Pir_Type) = "ptr" then
                                          Next_Reg (Ctx.E, "ep2i")
                                       else
                                          Emit_Int_Convert (Ctx, To_String (Eff_Src.Reg), To_String (Eff_Src.Pir_Type), "i64"));
                                    TA   : constant String := Next_Reg (Ctx.E, "er2t");
                                    L_H  : constant String := Next_Label (Ctx.E, "er2_h");
                                    L_B  : constant String := Next_Label (Ctx.E, "er2_b");
                                    L_N  : constant String := Next_Label (Ctx.E, "er2_n");
                                    L_D  : constant String := Next_Label (Ctx.E, "er2_d");
                                    IR   : constant String := Next_Reg (Ctx.E, "er2i");
                                    Cnd  : constant String := Next_Reg (Ctx.E, "er2c");
                                    Off  : constant String := Next_Reg (Ctx.E, "er2o");
                                    EP   : constant String := Next_Reg (Ctx.E, "er2p");
                                    CV   : constant String := Next_Reg (Ctx.E, "er2v");
                                    Eq   : constant String := Next_Reg (Ctx.E, "er2e");
                                    Nx   : constant String := Next_Reg (Ctx.E, "er2x");
                                    PosR : constant String := Next_Reg (Ctx.E, "er2r");
                                    Fin  : Unbounded_String;
                                 begin
                                    if To_String (Eff_Src.Pir_Type) = "ptr" then
                                       Emit_Inst (Ctx.E, V64 & " = ptrtoint.i64 " & To_String (Eff_Src.Reg));
                                    end if;
                                    Emit_Inst (Ctx.E, TA & " = addr " & Tbl);
                                    Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                                    Emit_Label (Ctx.E, L_H, IR & ": i64");
                                    Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Img (Cnt));
                                    Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_D & "(0)");
                                    Emit_Label (Ctx.E, L_B);
                                    Emit_Inst (Ctx.E, Off & " = mul.i64 " & IR & ", 8 [unsafe_s]");
                                    Emit_Inst (Ctx.E, EP & " = ptr_add.i64 " & TA & ", " & Off & " [inbounds]");
                                    Emit_Inst (Ctx.E, CV & " = load.i64 " & EP & ", align 8");
                                    Emit_Inst (Ctx.E, Eq & " = cmp_eq.i64 " & CV & ", " & V64);
                                    Emit_Term (Ctx.E, "br " & Eq & ", " & L_D & "(" & IR & "), " & L_N);
                                    Emit_Label (Ctx.E, L_N);
                                    Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                                    Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                                    Emit_Label (Ctx.E, L_D, PosR & ": i64");
                                    Fin := To_Unbounded_String (Emit_Int_Convert (Ctx, PosR, "i64", To_String (RTI.Pir_Type)));
                                    Res.Kind := EK_Scalar; Res.Reg := Fin;
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                                 end;
                              elsif Src_Has_Rep and then To_String (RTI.Pir_Type) /= "ptr"
                                and then To_String (RTI.Pir_Type) /= "f32" and then To_String (RTI.Pir_Type) /= "f64"
                              then
                                 declare
                                    Fin : constant String := Emit_Int_Convert (Ctx, To_String (Eff_Src.Reg), "i64", To_String (RTI.Pir_Type));
                                 begin
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (Fin);
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                                 end;
                              elsif To_String (Eff_Src.Pir_Type) = To_String (RTI.Pir_Type) then
                                 Res := Eff_Src;
                                 Res.Ada_Type := Res_Type;
                              elsif To_String (Eff_Src.Pir_Type) = "ptr" then
                                 Emit_Inst (Ctx.E, R & " = ptrtoint." & To_String (RTI.Pir_Type) & " " & To_String (Eff_Src.Reg));
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                              elsif To_String (RTI.Pir_Type) = "ptr" then
                                 Emit_Inst (Ctx.E, R & " = inttoptr.ptr " & To_String (Eff_Src.Reg));
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                              elsif Eff_STI.Byte_Size /= RTI.Byte_Size then
                                 declare
                                    Buf : constant String := Next_Reg (Ctx.E, "ucb");
                                    Sz  : constant Natural := Natural'Max (Eff_STI.Byte_Size, RTI.Byte_Size);
                                    Al  : constant Natural := Natural'Max (Eff_STI.Align, RTI.Align);
                                 begin
                                    Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, Sz)) & " x i8], align " & Img (Natural'Max (1, Al)));
                                    Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (Natural'Max (1, Sz)) & ", align 1");
                                    Emit_Inst (Ctx.E, "store." & To_String (Eff_Src.Pir_Type) & " " & To_String (Eff_Src.Reg) & ", " & Buf & ", align " & Img (Natural'Max (1, Eff_STI.Align)));
                                    Emit_Inst (Ctx.E, R & " = load." & To_String (RTI.Pir_Type) & " " & Buf & ", align " & Img (Natural'Max (1, RTI.Align)));
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Natural'Max (1, RTI.Align);
                                 end;
                              else
                                 Emit_Inst (Ctx.E, R & " = bitcast." & To_String (RTI.Pir_Type) & " " & To_String (Eff_Src.Reg));
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                              end if;
                           end;
                        end if;
                        return Res;
                     end;
                  end if;
               end;
            end if;
         end;
      end if;

      -- Helper: extract (ptr, first, len) from an Ada String Expr_Val
      declare
      procedure Emit_Str_Ptr_Len
        (SV    : Expr_Val;
         Ptr_S : out Unbounded_String;
         F_S   : out Unbounded_String;
         Len_S : out Unbounded_String)
      is
         F, L : Unbounded_String;
         Df : constant String := Next_Reg (Ctx.E, "sd");
         Ln : constant String := Next_Reg (Ctx.E, "sl");
         Ps : constant String := Next_Reg (Ctx.E, "sp");
         Cl : constant String := Next_Reg (Ctx.E, "scl");
      begin
         Get_Array_Dim_Bounds (Ctx, SV, 1, F, L);
         Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L) & ", " & To_String (F));
         Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
         Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
         Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
         Ptr_S := SV.Reg;
         F_S   := F;
         Len_S := To_Unbounded_String (Cl);
      end Emit_Str_Ptr_Len;

      function Is_File_Arg (Arg_Node : TREE) return Boolean is
         ATy : constant TREE := (if not Is_Void_Tree (Arg_Node) and then KIND (Arg_Node) in CLASS_EXP
                                 then Subst_Tree (Ctx, D (SM_EXP_TYPE, Arg_Node)) else CONST_VOID);
      begin
         return not Is_Void_Tree (ATy) and then Is_Record_Type (ATy);
      end Is_File_Arg;

      function Is_Str_Arg (Arg_Node : TREE) return Boolean is
         ATy : constant TREE := (if not Is_Void_Tree (Arg_Node) and then KIND (Arg_Node) in CLASS_EXP
                                 then Subst_Tree (Ctx, D (SM_EXP_TYPE, Arg_Node)) else CONST_VOID);
      begin
         if not Is_Void_Tree (Arg_Node) and then KIND (Arg_Node) = DN_STRING_LITERAL then
            return True;
         end if;
         return not Is_Void_Tree (ATy) and then Is_String_Type (ATy);
      end Is_Str_Arg;

      function Emit_File_Ptr_Or_Null (Has_File : Boolean) return String is
      begin
         if Has_File then
            declare
               FV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
            begin
               return To_String (FV.Reg);
            end;
         end if;
         return "0";
      end Emit_File_Ptr_Or_Null;

      -- Check if enclosing package is an instantiation of TEXT_IO.{INTEGER,FLOAT,FIXED,ENUMERATION}_IO,
      -- SEQUENTIAL_IO, or DIRECT_IO
      begin
      declare
         Reg : constant TREE := (if KIND (Def) in CLASS_SOURCE_NAME then D (XD_REGION, Def) else CONST_VOID);
      begin
         if not Is_Void_Tree (Reg) and then KIND (Reg) = DN_PACKAGE_ID then
            declare
               PUD : constant TREE := D (SM_UNIT_DESC, Reg);
            begin
               if not Is_Void_Tree (PUD) and then KIND (PUD) = DN_INSTANTIATION then
                  declare
                     Gen_P : constant TREE := Get_Name_Defn (D (AS_NAME, PUD));
                     GP_N  : constant String := Sym_Name (Gen_P);
                  begin
                     -- Extract instantiated generic parameter type (first DN_SUBTYPE_DECL in SM_DECL_S)
                     if (GP_N = "INTEGER_IO" or else GP_N = "FLOAT_IO"
                         or else GP_N = "FIXED_IO" or else GP_N = "ENUMERATION_IO")
                       and then not Is_Void_Tree (Gen_P)
                       and then KIND (Gen_P) in CLASS_SOURCE_NAME
                       and then not Is_Void_Tree (D (XD_REGION, Gen_P))
                       and then Sym_Name (D (XD_REGION, Gen_P)) = "TEXT_IO"
                       and then Is_Library_Level (D (XD_REGION, Gen_P))
                     then
                        declare
                           Inst_T : TREE := CONST_VOID;
                           DS     : constant TREE := D (SM_DECL_S, PUD);

                           procedure Emit_Field_Check (R32 : String) is
                           begin
                              if Ctx.Enable_Checks then
                                 declare
                                    Bad : constant String := Next_Reg (Ctx.E, "iofb");
                                 begin
                                    Emit_Inst (Ctx.E, Bad & " = cmp_lt_s.i32 " & R32 & ", 0");
                                    Emit_Check_Fail_If (Ctx, Bad);
                                 end;
                              end if;
                           end Emit_Field_Check;

                           procedure Emit_Base_Check (R32 : String) is
                           begin
                              if Ctx.Enable_Checks then
                                 declare
                                    LoC : constant String := Next_Reg (Ctx.E, "iob1");
                                    HiC : constant String := Next_Reg (Ctx.E, "iob2");
                                    Bad : constant String := Next_Reg (Ctx.E, "iobb");
                                 begin
                                    Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i32 " & R32 & ", 2");
                                    Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i32 " & R32 & ", 16");
                                    Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                                    Emit_Check_Fail_If (Ctx, Bad);
                                 end;
                              end if;
                           end Emit_Base_Check;

                           procedure Emit_Raise_Data_If (Cond_R : String) is
                              L_Fail : constant String := Next_Label (Ctx.E, "iode");
                              L_Ok   : constant String := Next_Label (Ctx.E, "iook");
                              Dummy  : Unbounded_String;
                           begin
                              Emit_Term (Ctx.E, "br " & Cond_R & ", " & L_Fail & ", " & L_Ok);
                              Emit_Label (Ctx.E, L_Fail);
                              Emit_Call_Or_Invoke (Ctx, "@__ada_raise_data_error", "", "", Dummy);
                              Emit_Term (Ctx.E, "unreachable");
                              Emit_Label (Ctx.E, L_Ok);
                           end Emit_Raise_Data_If;

                           procedure Emit_Inst_Range_Data_Check (Val_R : String; Pir_T : String; Check_T : TREE) is
                              RT : constant TREE := Resolve_Type_Spec (Check_T);
                           begin
                              if Is_Void_Tree (RT) or else not Ctx.Enable_Checks then
                                 return;
                              end if;
                              if Is_Discrete_Type (RT) then
                                 declare
                                    St : Boolean;
                                    Lo, Hi : Integer_64;
                                 begin
                                    Get_Discrete_Bounds (RT, St, Lo, Hi);
                                    if St then
                                       declare
                                          LoC : constant String := Next_Reg (Ctx.E, "id1");
                                          HiC : constant String := Next_Reg (Ctx.E, "id2");
                                          Bad : constant String := Next_Reg (Ctx.E, "idb");
                                       begin
                                          Emit_Inst (Ctx.E, LoC & " = cmp_lt_s." & Pir_T & " " & Val_R & ", " & Img (Long_Long_Integer (Lo)));
                                          Emit_Inst (Ctx.E, HiC & " = cmp_gt_s." & Pir_T & " " & Val_R & ", " & Img (Long_Long_Integer (Hi)));
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                                          Emit_Raise_Data_If (Bad);
                                       end;
                                    elsif Ctx.Saved_Type_Ranges.Contains (Tree_Key (RT))
                                      and then (Ctx.Saved_Type_Ranges.Element (Tree_Key (RT)).Is_Global
                                                or else Ctx.Saved_Type_Ranges.Element (Tree_Key (RT)).Owner_Subp = Ctx.Cur_Subp_Key)
                                    then
                                       declare
                                          SR  : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (RT));
                                          LR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                                          HR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                                          L_R : constant String := Emit_Int_Convert (Ctx, LR0, To_String (SR.Pir_Type), Pir_T);
                                          H_R : constant String := Emit_Int_Convert (Ctx, HR0, To_String (SR.Pir_Type), Pir_T);
                                          LoC : constant String := Next_Reg (Ctx.E, "id1");
                                          HiC : constant String := Next_Reg (Ctx.E, "id2");
                                          Bad : constant String := Next_Reg (Ctx.E, "idb");
                                       begin
                                          Emit_Inst (Ctx.E, LoC & " = cmp_lt_s." & Pir_T & " " & Val_R & ", " & L_R);
                                          Emit_Inst (Ctx.E, HiC & " = cmp_gt_s." & Pir_T & " " & Val_R & ", " & H_R);
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                                          Emit_Raise_Data_If (Bad);
                                       end;
                                    elsif KIND (RT) in DN_INTEGER | DN_ENUMERATION
                                      and then not Is_Void_Tree (D (SM_RANGE, RT))
                                      and then KIND (D (SM_RANGE, RT)) = DN_RANGE
                                    then
                                       declare
                                          Rng : constant TREE := D (SM_RANGE, RT);
                                          E1  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng), RT);
                                          E2  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng), RT);
                                          L_R : constant String := Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), Pir_T);
                                          H_R : constant String := Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), Pir_T);
                                          LoC : constant String := Next_Reg (Ctx.E, "id1");
                                          HiC : constant String := Next_Reg (Ctx.E, "id2");
                                          Bad : constant String := Next_Reg (Ctx.E, "idb");
                                       begin
                                          Emit_Inst (Ctx.E, LoC & " = cmp_lt_s." & Pir_T & " " & Val_R & ", " & L_R);
                                          Emit_Inst (Ctx.E, HiC & " = cmp_gt_s." & Pir_T & " " & Val_R & ", " & H_R);
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                                          Emit_Raise_Data_If (Bad);
                                       end;
                                    end if;
                                 end;
                              elsif (Is_Float_Type (RT) or else Is_Fixed_Type (RT)) and then Pir_T = "f64" then
                                 declare
                                    Rng : constant TREE := D (SM_RANGE, RT);
                                 begin
                                    if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                                       declare
                                          E1 : constant TREE := D (AS_EXP1, Rng);
                                          E2 : constant TREE := D (AS_EXP2, Rng);
                                          LoF, HiF : IEEE_Float_64;
                                          Have_Lo, Have_Hi : Boolean := False;
                                       begin
                                          if Has_Static_Val (Safe_SM_Value (E1)) then
                                             LoF := Eval_Float64 (Safe_SM_Value (E1));
                                             Have_Lo := True;
                                          end if;
                                          if Has_Static_Val (Safe_SM_Value (E2)) then
                                             HiF := Eval_Float64 (Safe_SM_Value (E2));
                                             Have_Hi := True;
                                          end if;
                                          if Have_Lo and then Have_Hi then
                                             declare
                                                LoC : constant String := Next_Reg (Ctx.E, "fd1");
                                                HiC : constant String := Next_Reg (Ctx.E, "fd2");
                                                Bad : constant String := Next_Reg (Ctx.E, "fdb");
                                             begin
                                                Emit_Inst (Ctx.E, LoC & " = fcmp_olt.f64 " & Val_R & ", " & Format_Float64 (LoF));
                                                Emit_Inst (Ctx.E, HiC & " = fcmp_ogt.f64 " & Val_R & ", " & Format_Float64 (HiF));
                                                Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                                                Emit_Raise_Data_If (Bad);
                                             end;
                                          else
                                             declare
                                                EV1 : constant Expr_Val := Emit_Expr (Ctx, E1, RT);
                                                EV2 : constant Expr_Val := Emit_Expr (Ctx, E2, RT);
                                                L_R : Unbounded_String;
                                                H_R : Unbounded_String;
                                                LoC : constant String := Next_Reg (Ctx.E, "fd1");
                                                HiC : constant String := Next_Reg (Ctx.E, "fd2");
                                                Bad : constant String := Next_Reg (Ctx.E, "fdb");
                                             begin
                                                if Is_Fixed_Type (RT) then
                                                   declare
                                                      F1 : constant String := Next_Reg (Ctx.E, "fxl1");
                                                      S1 : constant String := Next_Reg (Ctx.E, "fxs1");
                                                      F2 : constant String := Next_Reg (Ctx.E, "fxl2");
                                                      S2 : constant String := Next_Reg (Ctx.E, "fxs2");
                                                   begin
                                                      Emit_Inst (Ctx.E, F1 & " = sitofp.f64 " & To_String (EV1.Reg));
                                                      Emit_Inst (Ctx.E, S1 & " = fmul.f64 " & F1 & ", " & Format_Float64 (Fixed_Small (RT)));
                                                      Emit_Inst (Ctx.E, F2 & " = sitofp.f64 " & To_String (EV2.Reg));
                                                      Emit_Inst (Ctx.E, S2 & " = fmul.f64 " & F2 & ", " & Format_Float64 (Fixed_Small (RT)));
                                                      L_R := To_Unbounded_String (S1);
                                                      H_R := To_Unbounded_String (S2);
                                                   end;
                                                else
                                                   L_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), "f64"));
                                                   H_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (EV2.Reg), To_String (EV2.Pir_Type), "f64"));
                                                end if;
                                                Emit_Inst (Ctx.E, LoC & " = fcmp_olt.f64 " & Val_R & ", " & To_String (L_R));
                                                Emit_Inst (Ctx.E, HiC & " = fcmp_ogt.f64 " & Val_R & ", " & To_String (H_R));
                                                Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                                                Emit_Raise_Data_If (Bad);
                                             end;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                           end Emit_Inst_Range_Data_Check;
                        begin
                           if not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS)) then
                              declare
                                 D1 : constant TREE := HEAD (LIST (DS));
                              begin
                                 if KIND (D1) = DN_SUBTYPE_DECL then
                                    Inst_T := Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (AS_SOURCE_NAME, D1)));
                                 end if;
                              end;
                           end if;

                        if Name = "PUT" then
                           Handled := True;
                           if Is_Str_Arg (Args (1)) then
                              -- PUT(TO : out STRING; ITEM : ...; ...)
                              declare
                                 Dst : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 DP, DF, DL : Unbounded_String;
                                 Dummy : Unbounded_String;
                              begin
                                 Emit_Str_Ptr_Len (Dst, DP, DF, DL);
                                 pragma Unreferenced (DF);
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       IV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                       BV  : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                       I64 : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i64");
                                       B32 : constant String := Emit_Int_Convert (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, I64, "i64", Inst_T);
                                       Emit_Base_Check (B32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_i64_str",
                                         To_String (DP) & ", " & To_String (DL) & ", " & I64 & ", " & B32, "", Dummy);
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       FV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                       AfV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                       ExV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
                                       F64 : Unbounded_String := FV.Reg;
                                       Af32 : constant String := Emit_Int_Convert (Ctx, To_String (AfV.Reg), To_String (AfV.Pir_Type), "i32");
                                       Ex32 : constant String := Emit_Int_Convert (Ctx, To_String (ExV.Reg), To_String (ExV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (FV.Reg), To_String (FV.Pir_Type), Inst_T);
                                       Emit_Field_Check (Af32);
                                       Emit_Field_Check (Ex32);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Fl : constant String := Next_Reg (Ctx.E, "fxf");
                                             Sc : constant String := Next_Reg (Ctx.E, "fxs");
                                          begin
                                             Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (FV.Reg));
                                             Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (FV.Ada_Type)));
                                             F64 := To_Unbounded_String (Sc);
                                          end;
                                       elsif To_String (FV.Pir_Type) = "f32" then
                                          F64 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (FV.Reg), "f32", "f64"));
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_f64_str",
                                         To_String (DP) & ", " & To_String (DL) & ", " & To_String (F64) & ", " & Af32 & ", " & Ex32, "", Dummy);
                                    end;
                                 else
                                    -- ENUMERATION_IO.PUT(TO, ITEM, SET)
                                    declare
                                       EV   : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                       SetV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                       BT   : constant TREE := Base_Type_Spec (EV.Ada_Type);
                                       PS   : Unbounded_String;
                                       Cnt  : Natural;
                                       Idx64 : constant String := Emit_Int_Convert (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), "i64");
                                       PA    : constant String := Next_Reg (Ctx.E, "epa");
                                       PE    : constant String := Next_Reg (Ctx.E, "epe");
                                       SP    : constant String := Next_Reg (Ctx.E, "esp");
                                       LC    : constant String := Next_Reg (Ctx.E, "elc");
                                       LC32  : constant String := Next_Reg (Ctx.E, "elc32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), Inst_T);
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       pragma Unreferenced (Cnt);
                                       Emit_Inst (Ctx.E, PA & " = addr " & To_String (PS));
                                       Emit_Inst (Ctx.E, PE & " = elem_ptr ptr, " & PA & ", " & Idx64);
                                       Emit_Inst (Ctx.E, SP & " = load.ptr " & PE & ", align 8");
                                       Emit_Inst (Ctx.E, LC & " = cmp_eq." & To_String (SetV.Pir_Type) & " " & To_String (SetV.Reg) & ", 0");
                                       Emit_Inst (Ctx.E, LC32 & " = zext.i32 " & LC);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_enum_str",
                                         To_String (DP) & ", " & To_String (DL) & ", " & SP & ", " & LC32, "", Dummy);
                                    end;
                                 end if;
                              end;
                              return Res;
                           else
                              -- PUT([FILE,] ITEM, ...)
                              declare
                                 Has_F : constant Boolean := Is_File_Arg (Args (1));
                                 Off   : constant Natural := (if Has_F then 1 else 0);
                                 FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
                                 Dummy : Unbounded_String;
                              begin
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       IV  : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
                                       WV  : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       BV  : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                                       I64 : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i64");
                                       W32 : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       B32 : constant String := Emit_Int_Convert (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, I64, "i64", Inst_T);
                                       Emit_Field_Check (W32);
                                       Emit_Base_Check (B32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_i64",
                                         FP & ", " & I64 & ", " & W32 & ", " & B32, "", Dummy);
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       FV  : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
                                       FrV : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       AfV : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                                       ExV : constant Expr_Val := Emit_Expr (Ctx, Args (4 + Off));
                                       F64 : Unbounded_String := FV.Reg;
                                       Fr32 : constant String := Emit_Int_Convert (Ctx, To_String (FrV.Reg), To_String (FrV.Pir_Type), "i32");
                                       Af32 : constant String := Emit_Int_Convert (Ctx, To_String (AfV.Reg), To_String (AfV.Pir_Type), "i32");
                                       Ex32 : constant String := Emit_Int_Convert (Ctx, To_String (ExV.Reg), To_String (ExV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (FV.Reg), To_String (FV.Pir_Type), Inst_T);
                                       Emit_Field_Check (Fr32);
                                       Emit_Field_Check (Af32);
                                       Emit_Field_Check (Ex32);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Fl : constant String := Next_Reg (Ctx.E, "fxf");
                                             Sc : constant String := Next_Reg (Ctx.E, "fxs");
                                          begin
                                             Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (FV.Reg));
                                             Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (FV.Ada_Type)));
                                             F64 := To_Unbounded_String (Sc);
                                          end;
                                       elsif To_String (FV.Pir_Type) = "f32" then
                                          F64 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (FV.Reg), "f32", "f64"));
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_f64",
                                         FP & ", " & To_String (F64) & ", " & Fr32 & ", " & Af32 & ", " & Ex32, "", Dummy);
                                    end;
                                 else
                                    -- ENUMERATION_IO.PUT([FILE,] ITEM, WIDTH, SET)
                                    declare
                                       EV   : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
                                       WV   : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       SetV : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                                       BT   : constant TREE := Base_Type_Spec (EV.Ada_Type);
                                       PS   : Unbounded_String;
                                       Cnt  : Natural;
                                       Idx64 : constant String := Emit_Int_Convert (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), "i64");
                                       W32   : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       PA    : constant String := Next_Reg (Ctx.E, "epa");
                                       PE    : constant String := Next_Reg (Ctx.E, "epe");
                                       SP    : constant String := Next_Reg (Ctx.E, "esp");
                                       LC    : constant String := Next_Reg (Ctx.E, "elc");
                                       LC32  : constant String := Next_Reg (Ctx.E, "elc32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), Inst_T);
                                       Emit_Field_Check (W32);
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       pragma Unreferenced (Cnt);
                                       Emit_Inst (Ctx.E, PA & " = addr " & To_String (PS));
                                       Emit_Inst (Ctx.E, PE & " = elem_ptr ptr, " & PA & ", " & Idx64);
                                       Emit_Inst (Ctx.E, SP & " = load.ptr " & PE & ", align 8");
                                       Emit_Inst (Ctx.E, LC & " = cmp_eq." & To_String (SetV.Pir_Type) & " " & To_String (SetV.Reg) & ", 0");
                                       Emit_Inst (Ctx.E, LC32 & " = zext.i32 " & LC);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_enum",
                                         FP & ", " & SP & ", " & W32 & ", " & LC32, "", Dummy);
                                    end;
                                 end if;
                              end;
                              return Res;
                           end if;
                        elsif Name = "GET" then
                           Handled := True;
                           if Is_Str_Arg (Args (1)) then
                              -- GET(FROM : in STRING; ITEM : out ...; LAST : out POSITIVE)
                              declare
                                 Src   : constant Expr_Val := Emit_Expr (Ctx, Args (1));
                                 SP, SF, SL : Unbounded_String;
                                 ItLV  : constant Expr_Val := Emit_LValue (Ctx, Args (2));
                                 LaLV  : constant Expr_Val := Emit_LValue (Ctx, Args (3));
                                 ITI   : constant Type_Info := Get_Type_Info (Ctx, ItLV.Ada_Type);
                                 LaTmp : constant String := Next_Reg (Ctx.E, "lat");
                                 LaVal : constant String := Next_Reg (Ctx.E, "lav");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, LaTmp & " = alloca i32, align 4");
                                 Emit_Str_Ptr_Len (Src, SP, SF, SL);
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       RV : Unbounded_String;
                                       Cv : Unbounded_String;
                                    begin
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_i64_str",
                                         To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & LaTmp, "i64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i64", Inst_T);
                                       Emit_Range_Check (Ctx, To_String (RV), "i64", ItLV.Ada_Type);
                                       Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                       Emit_Range_Check (Ctx, LaVal, "i32", LaLV.Ada_Type);
                                       Emit_Inst (Ctx.E, "store.i32 " & LaVal & ", " & To_String (LaLV.Reg) & ", align 4");
                                       Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i64", To_String (ITI.Pir_Type)));
                                       Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & To_String (Cv) & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       RV : Unbounded_String;
                                    begin
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_f64_str",
                                         To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & LaTmp, "f64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "f64", Inst_T);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Dv  : constant String := Next_Reg (Ctx.E, "fxd");
                                             Ge  : constant String := Next_Reg (Ctx.E, "fxge");
                                             Hlf : constant String := Next_Reg (Ctx.E, "fxhl");
                                             Rnd : constant String := Next_Reg (Ctx.E, "fxrn");
                                             Cv  : constant String := Next_Reg (Ctx.E, "fxc");
                                          begin
                                             Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & To_String (RV) & ", " & Format_Float64 (Fixed_Small (ItLV.Ada_Type)));
                                             Emit_Inst (Ctx.E, Ge & " = fcmp_oge.f64 " & Dv & ", 0.0");
                                             Emit_Inst (Ctx.E, Hlf & " = select.f64 " & Ge & ", 0.5, -0.5");
                                             Emit_Inst (Ctx.E, Rnd & " = fadd.f64 " & Dv & ", " & Hlf);
                                             Emit_Inst (Ctx.E, Cv & " = fptosi." & To_String (ITI.Pir_Type) & " " & Rnd & " [sat]");
                                             Emit_Range_Check (Ctx, Cv, To_String (ITI.Pir_Type), ItLV.Ada_Type);
                                             Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                             Emit_Range_Check (Ctx, LaVal, "i32", LaLV.Ada_Type);
                                             Emit_Inst (Ctx.E, "store.i32 " & LaVal & ", " & To_String (LaLV.Reg) & ", align 4");
                                             Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & Cv & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                          end;
                                       else
                                          declare
                                             Cv : constant String := Emit_Scalar_Coerce (Ctx, To_String (RV), "f64", To_String (ITI.Pir_Type));
                                          begin
                                             Emit_Range_Check (Ctx, Cv, To_String (ITI.Pir_Type), ItLV.Ada_Type);
                                             Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                             Emit_Range_Check (Ctx, LaVal, "i32", LaLV.Ada_Type);
                                             Emit_Inst (Ctx.E, "store.i32 " & LaVal & ", " & To_String (LaLV.Reg) & ", align 4");
                                             Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & Cv & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                          end;
                                       end if;
                                    end;
                                 else
                                    declare
                                       BT  : constant TREE := Base_Type_Spec (ItLV.Ada_Type);
                                       PS  : Unbounded_String;
                                       Cnt : Natural;
                                       TA  : constant String := Next_Reg (Ctx.E, "eta");
                                       RV  : Unbounded_String;
                                       Cv  : Unbounded_String;
                                       IsCh : constant String := (if Is_Character_Type (BT) then "1" else "0");
                                    begin
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       Emit_Inst (Ctx.E, TA & " = addr " & To_String (PS));
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_enum_str",
                                         To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & TA & ", " & Img (Cnt) & ", " & LaTmp & ", " & IsCh, "i32", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i32", Inst_T);
                                       Emit_Range_Check (Ctx, To_String (RV), "i32", ItLV.Ada_Type);
                                       Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                       Emit_Range_Check (Ctx, LaVal, "i32", LaLV.Ada_Type);
                                       Emit_Inst (Ctx.E, "store.i32 " & LaVal & ", " & To_String (LaLV.Reg) & ", align 4");
                                       Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", To_String (ITI.Pir_Type)));
                                       Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & To_String (Cv) & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                    end;
                                 end if;
                              end;
                              return Res;
                           else
                              -- GET([FILE,] ITEM [, WIDTH])
                              declare
                                 Has_F : constant Boolean := Is_File_Arg (Args (1));
                                 Off   : constant Natural := (if Has_F then 1 else 0);
                                 FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
                                 ItLV  : constant Expr_Val := Emit_LValue (Ctx, Args (1 + Off));
                                 ITI   : constant Type_Info := Get_Type_Info (Ctx, ItLV.Ada_Type);
                              begin
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       WV  : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       W32 : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       RV  : Unbounded_String;
                                       Cv  : Unbounded_String;
                                    begin
                                       Emit_Field_Check (W32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_i64", FP & ", " & W32, "i64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i64", Inst_T);
                                       Emit_Range_Check (Ctx, To_String (RV), "i64", ItLV.Ada_Type);
                                       Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i64", To_String (ITI.Pir_Type)));
                                       Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & To_String (Cv) & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       WV  : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       W32 : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       RV  : Unbounded_String;
                                    begin
                                       Emit_Field_Check (W32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_f64", FP & ", " & W32, "f64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "f64", Inst_T);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Dv  : constant String := Next_Reg (Ctx.E, "fxd");
                                             Ge  : constant String := Next_Reg (Ctx.E, "fxge");
                                             Hlf : constant String := Next_Reg (Ctx.E, "fxhl");
                                             Rnd : constant String := Next_Reg (Ctx.E, "fxrn");
                                             Cv  : constant String := Next_Reg (Ctx.E, "fxc");
                                          begin
                                             Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & To_String (RV) & ", " & Format_Float64 (Fixed_Small (ItLV.Ada_Type)));
                                             Emit_Inst (Ctx.E, Ge & " = fcmp_oge.f64 " & Dv & ", 0.0");
                                             Emit_Inst (Ctx.E, Hlf & " = select.f64 " & Ge & ", 0.5, -0.5");
                                             Emit_Inst (Ctx.E, Rnd & " = fadd.f64 " & Dv & ", " & Hlf);
                                             Emit_Inst (Ctx.E, Cv & " = fptosi." & To_String (ITI.Pir_Type) & " " & Rnd & " [sat]");
                                             Emit_Range_Check (Ctx, Cv, To_String (ITI.Pir_Type), ItLV.Ada_Type);
                                             Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & Cv & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                          end;
                                       else
                                          declare
                                             Cv : constant String := Emit_Scalar_Coerce (Ctx, To_String (RV), "f64", To_String (ITI.Pir_Type));
                                          begin
                                             Emit_Range_Check (Ctx, Cv, To_String (ITI.Pir_Type), ItLV.Ada_Type);
                                             Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & Cv & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                          end;
                                       end if;
                                    end;
                                 else
                                    declare
                                       BT  : constant TREE := Base_Type_Spec (ItLV.Ada_Type);
                                       PS  : Unbounded_String;
                                       Cnt : Natural;
                                       TA   : constant String := Next_Reg (Ctx.E, "eta");
                                       RV   : Unbounded_String;
                                       Cv   : Unbounded_String;
                                       IsCh : constant String := (if Is_Character_Type (BT) then "1" else "0");
                                    begin
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       Emit_Inst (Ctx.E, TA & " = addr " & To_String (PS));
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_enum", FP & ", " & TA & ", " & Img (Cnt) & ", " & IsCh, "i32", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i32", Inst_T);
                                       Emit_Range_Check (Ctx, To_String (RV), "i32", ItLV.Ada_Type);
                                       Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", To_String (ITI.Pir_Type)));
                                       Emit_Inst (Ctx.E, "store." & To_String (ITI.Pir_Type) & " " & To_String (Cv) & ", " & To_String (ItLV.Reg) & ", align " & Img (ITI.Align));
                                    end;
                                 end if;
                              end;
                              return Res;
                           end if;
                        end if;
                        end;

                     elsif (GP_N = "SEQUENTIAL_IO" or else GP_N = "DIRECT_IO")
                       and then not Is_Void_Tree (Gen_P) and then Is_Library_Level (Gen_P)
                     then
                        -- Extract ELEMENT_TYPE byte size from instantiation SM_DECL_S
                        declare
                           Is_Dir  : constant Boolean := GP_N = "DIRECT_IO";
                           Kind_I  : constant String := (if Is_Dir then "3" else "2");
                           Elem_Sz : Natural := 8;
                           Bio_Elem_T : TREE := CONST_VOID;
                           Is_Uncon_Arr : Boolean := False;
                           Arr_NDims    : Natural := 1;
                           Arr_Comp_Sz  : Natural := 1;
                           DS      : constant TREE := D (SM_DECL_S, PUD);

                           function Emit_Bio_Item_Size (V : Expr_Val) return String is
                           begin
                              if not Is_Uncon_Arr then
                                 return Img (Elem_Sz);
                              end if;
                              declare
                                 Total : Unbounded_String := To_Unbounded_String (Next_Reg (Ctx.E, "bsz0"));
                              begin
                                 Emit_Inst (Ctx.E, To_String (Total) & " = const.i64 " & Img (Arr_Comp_Sz));
                                 for D_Idx in 1 .. Arr_NDims loop
                                    declare
                                       F, L : Unbounded_String;
                                       Df   : constant String := Next_Reg (Ctx.E, "bdf");
                                       Ln   : constant String := Next_Reg (Ctx.E, "bln");
                                       Ps   : constant String := Next_Reg (Ctx.E, "bps");
                                       Cl   : constant String := Next_Reg (Ctx.E, "bcl");
                                       Cl64 : constant String := Next_Reg (Ctx.E, "bc6");
                                       Nx   : constant String := Next_Reg (Ctx.E, "bnx");
                                    begin
                                       Get_Array_Dim_Bounds (Ctx, V, D_Idx, F, L);
                                       Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L) & ", " & To_String (F));
                                       Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                                       Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                                       Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                                       Emit_Inst (Ctx.E, Cl64 & " = sext.i64 " & Cl);
                                       Emit_Inst (Ctx.E, Nx & " = mul.i64 " & To_String (Total) & ", " & Cl64);
                                       Total := To_Unbounded_String (Nx);
                                    end;
                                 end loop;
                                 return To_String (Total);
                              end;
                           end Emit_Bio_Item_Size;
                        begin
                           if not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS)) then
                              declare
                                 D1 : constant TREE := HEAD (LIST (DS));
                              begin
                                 if KIND (D1) = DN_SUBTYPE_DECL then
                                    declare
                                       ET : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (AS_SOURCE_NAME, D1)));
                                       RT : constant TREE := Resolve_Type_Spec (ET);
                                       ETI : constant Type_Info := Get_Type_Info (Ctx, ET);
                                    begin
                                       Elem_Sz := Natural'Max (1, ETI.Byte_Size);
                                       Bio_Elem_T := ET;
                                       if not Is_Void_Tree (RT) and then KIND (RT) = DN_ARRAY then
                                          Is_Uncon_Arr := True;
                                          Arr_NDims := Natural'Max (1, ETI.NDims);
                                          declare
                                             CT : constant TREE := Resolve_Type_Spec (D (SM_COMP_TYPE, RT));
                                             CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                          begin
                                             Arr_Comp_Sz := Natural'Max (1, CTI.Byte_Size);
                                          end;
                                       end if;
                                    end;
                                 end if;
                              end;
                           end if;

                           Handled := True;
                           if Name = "CREATE" or else Name = "OPEN" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                 M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
                                 NV  : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                 FoV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
                                 NP, NF, NL, FoP, FoF, FoL : Unbounded_String;
                                 Dummy : Unbounded_String;
                              begin
                                 Emit_Str_Ptr_Len (NV, NP, NF, NL);
                                 Emit_Str_Ptr_Len (FoV, FoP, FoF, FoL);
                                 pragma Unreferenced (NF, FoF);
                                 if Name = "CREATE" then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_bio_create",
                                      To_String (FLV.Reg) & ", " & Kind_I & ", " & M32 & ", " &
                                      To_String (NP) & ", " & To_String (NL) & ", " & To_String (FoP) & ", " & To_String (FoL) & ", " & Img (Elem_Sz), "", Dummy);
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_bio_open",
                                      To_String (FLV.Reg) & ", " & Kind_I & ", " & M32 & ", " &
                                      To_String (NP) & ", " & To_String (NL) & ", " & To_String (FoP) & ", " & To_String (FoL) & ", " & Img (Elem_Sz), "", Dummy);
                                 end if;
                              end;
                           elsif Name = "CLOSE" or else Name = "DELETE" or else Name = "RESET" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 Dummy : Unbounded_String;
                              begin
                                 if Name = "CLOSE" then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_tio_close", To_String (FLV.Reg), "", Dummy);
                                 elsif Name = "DELETE" then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_tio_delete", To_String (FLV.Reg), "", Dummy);
                                 elsif NArg = 2 then
                                    declare
                                       MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                       M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
                                    begin
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset_mode", To_String (FLV.Reg) & ", " & M32, "", Dummy);
                                    end;
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset", To_String (FLV.Reg), "", Dummy);
                                 end if;
                              end;
                           elsif Name = "IS_OPEN" or else Name = "MODE" or else Name = "END_OF_FILE" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 Fn  : constant String :=
                                   (if Name = "IS_OPEN" then "@__ada_tio_is_open"
                                    elsif Name = "MODE" then "@__ada_tio_mode"
                                    elsif Is_Dir then "@__ada_dir_eof"
                                    else "@__ada_seq_eof");
                                 RV  : Unbounded_String;
                              begin
                                 Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "i8", RV);
                                 Res.Kind := EK_Scalar; Res.Reg := RV;
                                 Res.Pir_Type := To_Unbounded_String ("i8");
                                 Res.Ada_Type := Res_Type; Res.Align := 1;
                              end;
                           elsif Name = "NAME" or else Name = "FORM" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 LP  : constant String := Next_Reg (Ctx.E, "nflp");
                                 LV  : constant String := Next_Reg (Ctx.E, "nfl");
                                 One : constant String := Next_Reg (Ctx.E, "nf1");
                                 Fn  : constant String := (if Name = "NAME" then "@__ada_tio_name" else "@__ada_tio_form");
                                 RV  : Unbounded_String;
                              begin
                                 Emit_Entry_Alloca (Ctx.E, LP & " = alloca i32, align 4");
                                 Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg) & ", " & LP, "ptr", RV);
                                 Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
                                 Emit_Inst (Ctx.E, One & " = const.i32 1");
                                 Res.Kind := EK_Slice; Res.Reg := RV; Res.Pir_Type := To_Unbounded_String ("ptr");
                                 Res.Ada_Type := Res_Type; Res.Align := 1; Res.NDims := 1;
                                 Res.First_Regs (1) := To_Unbounded_String (One);
                                 Res.Last_Regs (1)  := To_Unbounded_String (LV);
                              end;
                           elsif Name = "READ" then
                              declare
                                 FLV  : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 ItLV : constant Expr_Val := Emit_LValue (Ctx, Args (2));
                                 Sz64 : constant String := Emit_Bio_Item_Size (ItLV);
                                 Dummy : Unbounded_String;
                              begin
                                 if Is_Dir then
                                    declare
                                       Fr64   : Unbounded_String := To_Unbounded_String ("0");
                                       HasPos : constant String := (if NArg = 3 then "1" else "0");
                                    begin
                                       if NArg = 3 then
                                          declare
                                             FrV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                          begin
                                             Fr64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (FrV.Reg), To_String (FrV.Pir_Type), "i64"));
                                          end;
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_dir_read",
                                         To_String (FLV.Reg) & ", " & To_String (ItLV.Reg) & ", " & Sz64 & ", " & To_String (Fr64) & ", " & HasPos, "", Dummy);
                                    end;
                                 elsif Is_Uncon_Arr then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_read_framed",
                                      To_String (FLV.Reg) & ", " & To_String (ItLV.Reg) & ", " & Sz64, "", Dummy);
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_read",
                                      To_String (FLV.Reg) & ", " & To_String (ItLV.Reg) & ", " & Sz64, "", Dummy);
                                 end if;
                              end;
                           elsif Name = "WRITE" then
                              declare
                                 FLV  : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 ItV  : constant Expr_Val := Emit_Expr (Ctx, Args (2), Bio_Elem_T);
                                 Sz64 : constant String := Emit_Bio_Item_Size (ItV);
                                 ItP  : Unbounded_String := ItV.Reg;
                                 Dummy : Unbounded_String;
                              begin
                                 if ItV.Kind = EK_Scalar then
                                    declare
                                       Tmp : constant String := Next_Reg (Ctx.E, "biow");
                                    begin
                                       Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (ItV.Pir_Type) & ", align " & Img (ItV.Align));
                                       Emit_Inst (Ctx.E, "store." & To_String (ItV.Pir_Type) & " " & To_String (ItV.Reg) & ", " & Tmp & ", align " & Img (ItV.Align));
                                       ItP := To_Unbounded_String (Tmp);
                                    end;
                                 end if;
                                 if Is_Dir then
                                    declare
                                       To64   : Unbounded_String := To_Unbounded_String ("0");
                                       HasPos : constant String := (if NArg = 3 then "1" else "0");
                                    begin
                                       if NArg = 3 then
                                          declare
                                             ToV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                          begin
                                             To64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (ToV.Reg), To_String (ToV.Pir_Type), "i64"));
                                          end;
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_dir_write",
                                         To_String (FLV.Reg) & ", " & To_String (ItP) & ", " & Sz64 & ", " & To_String (To64) & ", " & HasPos, "", Dummy);
                                    end;
                                 elsif Is_Uncon_Arr then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_write_framed",
                                      To_String (FLV.Reg) & ", " & To_String (ItP) & ", " & Sz64, "", Dummy);
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_write",
                                      To_String (FLV.Reg) & ", " & To_String (ItP) & ", " & Sz64, "", Dummy);
                                 end if;
                              end;
                           elsif Name = "SET_INDEX" then
                              declare
                                 FLV  : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 ToV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                 To64 : constant String := Emit_Int_Convert (Ctx, To_String (ToV.Reg), To_String (ToV.Pir_Type), "i64");
                                 Dummy : Unbounded_String;
                              begin
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_dir_set_index", To_String (FLV.Reg) & ", " & To64, "", Dummy);
                              end;
                           elsif Name = "INDEX" or else Name = "SIZE" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 Fn  : constant String := (if Name = "INDEX" then "@__ada_dir_index" else "@__ada_dir_size");
                                 RV  : Unbounded_String;
                              begin
                                 Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "i32", RV);
                                 Res.Kind := EK_Scalar; Res.Reg := RV;
                                 Res.Pir_Type := To_Unbounded_String ("i32");
                                 Res.Ada_Type := Res_Type; Res.Align := 4;
                              end;
                           else
                              Handled := False;
                           end if;
                           if Handled then
                              return Res;
                           end if;
                        end;
                     end if;
                  end;
               end if;
            end;
         end if;
      end;

      if Pkg = "LOW_LEVEL_IO" and then Is_Library_Level (Def) then
         Handled := True;
         if Name = "SEND_CONTROL" or else Name = "RECEIVE_CONTROL" then
            declare
               DevV  : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               Dev32 : constant String := Emit_Int_Convert (Ctx, To_String (DevV.Reg), To_String (DevV.Pir_Type), "i32");
               DatLV : constant Expr_Val := Emit_LValue (Ctx, Args (2));
               Fn    : constant String := (if Name = "SEND_CONTROL" then "@__ada_llio_send" else "@__ada_llio_recv");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, Dev32 & ", " & To_String (DatLV.Reg), "", Dummy, Nounwind => True);
            end;
         else
            Handled := False;
         end if;
         if Handled then
            return Res;
         end if;
      end if;

      if Pkg = "TEXT_IO" and then Is_Library_Level (Def) then
         Handled := True;
         if Name = "CREATE" or else Name = "OPEN" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
               M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
               NV  : constant Expr_Val := Emit_Expr (Ctx, Args (3));
               FoV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
               NP, NF, NL, FoP, FoF, FoL : Unbounded_String;
               Fn  : constant String := (if Name = "CREATE" then "@__ada_tio_create" else "@__ada_tio_open");
               Dummy : Unbounded_String;
            begin
               Emit_Str_Ptr_Len (NV, NP, NF, NL);
               Emit_Str_Ptr_Len (FoV, FoP, FoF, FoL);
               pragma Unreferenced (NF, FoF);
               Emit_Call_Or_Invoke (Ctx, Fn,
                 To_String (FLV.Reg) & ", " & M32 & ", " &
                 To_String (NP) & ", " & To_String (NL) & ", " &
                 To_String (FoP) & ", " & To_String (FoL), "", Dummy);
            end;
         elsif Name = "CLOSE" or else Name = "DELETE" or else Name = "RESET" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               Dummy : Unbounded_String;
            begin
               if Name = "CLOSE" then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_close", To_String (FLV.Reg), "", Dummy);
               elsif Name = "DELETE" then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_delete", To_String (FLV.Reg), "", Dummy);
               elsif NArg = 2 then
                  declare
                     MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                     M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
                  begin
                     Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset_mode", To_String (FLV.Reg) & ", " & M32, "", Dummy);
                  end;
               else
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset", To_String (FLV.Reg), "", Dummy);
               end if;
            end;
         elsif Name = "IS_OPEN" or else Name = "MODE" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               Fn  : constant String := (if Name = "IS_OPEN" then "@__ada_tio_is_open" else "@__ada_tio_mode");
               RV  : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "i8", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Res_Type; Res.Align := 1;
            end;
         elsif Name = "NAME" or else Name = "FORM" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               LP  : constant String := Next_Reg (Ctx.E, "nflp");
               LV  : constant String := Next_Reg (Ctx.E, "nfl");
               One : constant String := Next_Reg (Ctx.E, "nf1");
               Fn  : constant String := (if Name = "NAME" then "@__ada_tio_name" else "@__ada_tio_form");
               RV  : Unbounded_String;
            begin
               Emit_Entry_Alloca (Ctx.E, LP & " = alloca i32, align 4");
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg) & ", " & LP, "ptr", RV);
               Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
               Emit_Inst (Ctx.E, One & " = const.i32 1");
               Res.Kind := EK_Slice; Res.Reg := RV; Res.Pir_Type := To_Unbounded_String ("ptr");
               Res.Ada_Type := Res_Type; Res.Align := 1; Res.NDims := 1;
               Res.First_Regs (1) := To_Unbounded_String (One);
               Res.Last_Regs (1)  := To_Unbounded_String (LV);
            end;
         elsif Name = "SET_INPUT" or else Name = "SET_OUTPUT" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               Fn  : constant String := (if Name = "SET_INPUT" then "@__ada_tio_set_input" else "@__ada_tio_set_output");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "", Dummy);
            end;
         elsif Name = "STANDARD_OUTPUT" or else Name = "CURRENT_OUTPUT"
           or else Name = "STANDARD_INPUT" or else Name = "CURRENT_INPUT"
         then
            declare
               RTI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
               Tmp : constant String := Next_Reg (Ctx.E, "ft");
               FP  : constant String := Next_Reg (Ctx.E, "ftf");
               Fn  : constant String :=
                 (if Name = "STANDARD_INPUT" then "@__ada_tio_standard_input"
                  elsif Name = "STANDARD_OUTPUT" then "@__ada_tio_standard_output"
                  elsif Name = "CURRENT_INPUT" then "@__ada_tio_current_input"
                  else "@__ada_tio_current_output");
               HV  : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, "", "i32", HV, Nounwind => True);
               Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (RTI.Pir_Type) & ", align " & Img (RTI.Align));
               Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & Tmp & ", 0");
               Emit_Inst (Ctx.E, "store.i32 " & To_String (HV) & ", " & FP & ", align 4");
               Res.Kind     := EK_Addr;
               Res.Reg      := To_Unbounded_String (Tmp);
               Res.Pir_Type := RTI.Pir_Type;
               Res.Ada_Type := Res_Type;
               Res.Align    := RTI.Align;
            end;
         elsif Name = "SET_LINE_LENGTH" or else Name = "SET_PAGE_LENGTH"
           or else Name = "SET_COL" or else Name = "SET_LINE"
           or else Name = "NEW_LINE" or else Name = "SKIP_LINE"
         then
            declare
               Has_F : constant Boolean := (NArg = 2);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               V     : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
               V32   : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i32");
               Fn    : constant String :=
                 (if Name = "SET_LINE_LENGTH" then "@__ada_tio_set_line_length"
                  elsif Name = "SET_PAGE_LENGTH" then "@__ada_tio_set_page_length"
                  elsif Name = "SET_COL" then "@__ada_tio_set_col"
                  elsif Name = "SET_LINE" then "@__ada_tio_set_line"
                  elsif Name = "NEW_LINE" then "@__ada_tio_new_line"
                  else "@__ada_tio_skip_line");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP & ", " & V32, "", Dummy);
            end;
         elsif Name = "NEW_PAGE" or else Name = "SKIP_PAGE" then
            declare
               FP : constant String := Emit_File_Ptr_Or_Null (NArg = 1);
               Fn : constant String := (if Name = "NEW_PAGE" then "@__ada_tio_new_page" else "@__ada_tio_skip_page");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP, "", Dummy);
            end;
         elsif Name = "LINE_LENGTH" or else Name = "PAGE_LENGTH"
           or else Name = "COL" or else Name = "LINE" or else Name = "PAGE"
         then
            declare
               FP : constant String := Emit_File_Ptr_Or_Null (NArg = 1);
               Fn : constant String :=
                 (if Name = "LINE_LENGTH" then "@__ada_tio_line_length"
                  elsif Name = "PAGE_LENGTH" then "@__ada_tio_page_length"
                  elsif Name = "COL" then "@__ada_tio_col"
                  elsif Name = "LINE" then "@__ada_tio_line"
                  else "@__ada_tio_page");
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP, "i32", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i32");
               Res.Ada_Type := Res_Type; Res.Align := 4;
            end;
         elsif Name = "END_OF_LINE" or else Name = "END_OF_PAGE" or else Name = "END_OF_FILE" then
            declare
               FP : constant String := Emit_File_Ptr_Or_Null (NArg = 1);
               Fn : constant String :=
                 (if Name = "END_OF_LINE" then "@__ada_tio_end_of_line"
                  elsif Name = "END_OF_PAGE" then "@__ada_tio_end_of_page"
                  else "@__ada_tio_end_of_file");
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP, "i8", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Res_Type; Res.Align := 1;
            end;
         elsif Name = "PUT" or else Name = "PUT_LINE" then
            declare
               Has_F : constant Boolean := (NArg = 2);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               V     : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
               Dummy : Unbounded_String;
            begin
               if Is_Character_Type (V.Ada_Type) then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_char", FP & ", " & To_String (V.Reg), "", Dummy);
               else
                  declare
                     SP, SF, SL : Unbounded_String;
                     Fn : constant String := (if Name = "PUT_LINE" then "@__ada_tio_put_line" else "@__ada_tio_put_str");
                  begin
                     Emit_Str_Ptr_Len (V, SP, SF, SL);
                     pragma Unreferenced (SF);
                     Emit_Call_Or_Invoke (Ctx, Fn, FP & ", " & To_String (SP) & ", " & To_String (SL), "", Dummy);
                  end;
               end if;
            end;
         elsif Name = "GET" then
            declare
               Has_F : constant Boolean := (NArg = 2);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               LV    : constant Expr_Val := Emit_LValue (Ctx, Args (1 + Off));
               Dummy : Unbounded_String;
            begin
               if Is_Character_Type (LV.Ada_Type) then
                  declare
                     RV : Unbounded_String;
                  begin
                     Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_char", FP, "i8", RV);
                     Emit_Inst (Ctx.E, "store.i8 " & To_String (RV) & ", " & To_String (LV.Reg) & ", align 1");
                  end;
               else
                  declare
                     SP, SF, SL : Unbounded_String;
                  begin
                     Emit_Str_Ptr_Len (LV, SP, SF, SL);
                     pragma Unreferenced (SF);
                     Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_str", FP & ", " & To_String (SP) & ", " & To_String (SL), "", Dummy);
                  end;
               end if;
            end;
         elsif Name = "GET_LINE" then
            declare
               Has_F : constant Boolean := (NArg = 3);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               SLV   : constant Expr_Val := Emit_LValue (Ctx, Args (1 + Off));
               LaLV  : constant Expr_Val := Emit_LValue (Ctx, Args (2 + Off));
               SP, SF, SL : Unbounded_String;
               Dummy : Unbounded_String;
            begin
               Emit_Str_Ptr_Len (SLV, SP, SF, SL);
               Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_line",
                 FP & ", " & To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & To_String (LaLV.Reg), "", Dummy);
            end;
         else
            Handled := False;
         end if;
         if Handled then
            return Res;
         end if;
      elsif Pkg = "CALENDAR" and then Is_Library_Level (Def) then
         if Name = "CLOCK" then
            Handled := True;
            declare
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_clock_ns", "", "i64", RV, Nounwind => True);
               Res.Kind     := EK_Scalar;
               Res.Reg      := RV;
               Res.Pir_Type := To_Unbounded_String ("i64");
               Res.Ada_Type := Res_Type;
               Res.Align    := 8;
            end;
            return Res;
         elsif Name = "YEAR" or else Name = "MONTH" or else Name = "DAY" then
            Handled := True;
            declare
               TV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               Fn : constant String :=
                 (if Name = "YEAR" then "@__ada_cal_year"
                  elsif Name = "MONTH" then "@__ada_cal_month"
                  else "@__ada_cal_day");
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (TV.Reg), "i32", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i32");
               Res.Ada_Type := Res_Type; Res.Align := 4;
            end;
            return Res;
         elsif Name = "SECONDS" then
            Handled := True;
            declare
               TV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_cal_seconds", To_String (TV.Reg), "i64", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i64");
               Res.Ada_Type := Res_Type; Res.Align := 8;
            end;
            return Res;
         elsif Name = "TIME_OF" then
            Handled := True;
            declare
               YV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               MV : constant Expr_Val := Emit_Expr (Ctx, Args (2));
               DV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
               SV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
               Y32 : constant String := Emit_Int_Convert (Ctx, To_String (YV.Reg), To_String (YV.Pir_Type), "i32");
               M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
               D32 : constant String := Emit_Int_Convert (Ctx, To_String (DV.Reg), To_String (DV.Pir_Type), "i32");
               S64 : constant String := Emit_Int_Convert (Ctx, To_String (SV.Reg), To_String (SV.Pir_Type), "i64");
               RV  : Unbounded_String;
            begin
               -- Subtype range checks for YEAR_NUMBER (1901..2099), MONTH_NUMBER (1..12),
               -- DAY_NUMBER (1..31), DAY_DURATION (0.0..86400.0) raise CONSTRAINT_ERROR (LRM 9.6)
               if Ctx.Enable_Checks then
                  declare
                     YLo : constant String := Next_Reg (Ctx.E, "cy1");
                     YHi : constant String := Next_Reg (Ctx.E, "cy2");
                     YB  : constant String := Next_Reg (Ctx.E, "cyb");
                     MLo : constant String := Next_Reg (Ctx.E, "cm1");
                     MHi : constant String := Next_Reg (Ctx.E, "cm2");
                     MB  : constant String := Next_Reg (Ctx.E, "cmb");
                     DLo : constant String := Next_Reg (Ctx.E, "cd1");
                     DHi : constant String := Next_Reg (Ctx.E, "cd2");
                     DB  : constant String := Next_Reg (Ctx.E, "cdb");
                     SLo : constant String := Next_Reg (Ctx.E, "cs1");
                     SHi : constant String := Next_Reg (Ctx.E, "cs2");
                     SB  : constant String := Next_Reg (Ctx.E, "csb");
                     B1  : constant String := Next_Reg (Ctx.E, "cb1");
                     B2  : constant String := Next_Reg (Ctx.E, "cb2");
                     Bad : constant String := Next_Reg (Ctx.E, "cbad");
                  begin
                     Emit_Inst (Ctx.E, YLo & " = cmp_lt_s.i32 " & Y32 & ", 1901");
                     Emit_Inst (Ctx.E, YHi & " = cmp_gt_s.i32 " & Y32 & ", 2099");
                     Emit_Inst (Ctx.E, YB  & " = or.i8 " & YLo & ", " & YHi);
                     Emit_Inst (Ctx.E, MLo & " = cmp_lt_s.i32 " & M32 & ", 1");
                     Emit_Inst (Ctx.E, MHi & " = cmp_gt_s.i32 " & M32 & ", 12");
                     Emit_Inst (Ctx.E, MB  & " = or.i8 " & MLo & ", " & MHi);
                     Emit_Inst (Ctx.E, DLo & " = cmp_lt_s.i32 " & D32 & ", 1");
                     Emit_Inst (Ctx.E, DHi & " = cmp_gt_s.i32 " & D32 & ", 31");
                     Emit_Inst (Ctx.E, DB  & " = or.i8 " & DLo & ", " & DHi);
                     Emit_Inst (Ctx.E, SLo & " = cmp_lt_s.i64 " & S64 & ", 0");
                     Emit_Inst (Ctx.E, SHi & " = cmp_gt_s.i64 " & S64 & ", 86400000000000");
                     Emit_Inst (Ctx.E, SB  & " = or.i8 " & SLo & ", " & SHi);
                     Emit_Inst (Ctx.E, B1  & " = or.i8 " & YB & ", " & MB);
                     Emit_Inst (Ctx.E, B2  & " = or.i8 " & DB & ", " & SB);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & B1 & ", " & B2);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;
               Emit_Call_Or_Invoke (Ctx, "@__ada_cal_time_of", Y32 & ", " & M32 & ", " & D32 & ", " & S64, "i64", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i64");
               Res.Ada_Type := Res_Type; Res.Align := 8;
            end;
            return Res;
         elsif Name = "SPLIT" then
            Handled := True;
            declare
               TV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               YL : constant Expr_Val := Emit_LValue (Ctx, Args (2));
               ML : constant Expr_Val := Emit_LValue (Ctx, Args (3));
               DL : constant Expr_Val := Emit_LValue (Ctx, Args (4));
               SL : constant Expr_Val := Emit_LValue (Ctx, Args (5));
               YT : constant String := Next_Reg (Ctx.E, "csy");
               MT : constant String := Next_Reg (Ctx.E, "csm");
               DT : constant String := Next_Reg (Ctx.E, "csd");
               ST : constant String := Next_Reg (Ctx.E, "css");
               YV : constant String := Next_Reg (Ctx.E, "csyv");
               MV : constant String := Next_Reg (Ctx.E, "csmv");
               DV : constant String := Next_Reg (Ctx.E, "csdv");
               SV : constant String := Next_Reg (Ctx.E, "cssv");
               Dummy : Unbounded_String;
            begin
               Emit_Entry_Alloca (Ctx.E, YT & " = alloca i32, align 4");
               Emit_Entry_Alloca (Ctx.E, MT & " = alloca i32, align 4");
               Emit_Entry_Alloca (Ctx.E, DT & " = alloca i32, align 4");
               Emit_Entry_Alloca (Ctx.E, ST & " = alloca i64, align 8");
               Emit_Call_Or_Invoke (Ctx, "@__ada_cal_split",
                 To_String (TV.Reg) & ", " & YT & ", " & MT & ", " & DT & ", " & ST, "", Dummy);
               Emit_Inst (Ctx.E, YV & " = load.i32 " & YT & ", align 4");
               Emit_Inst (Ctx.E, MV & " = load.i32 " & MT & ", align 4");
               Emit_Inst (Ctx.E, DV & " = load.i32 " & DT & ", align 4");
               Emit_Inst (Ctx.E, SV & " = load.i64 " & ST & ", align 8");
               Emit_Range_Check (Ctx, YV, "i32", YL.Ada_Type);
               Emit_Range_Check (Ctx, MV, "i32", ML.Ada_Type);
               Emit_Range_Check (Ctx, DV, "i32", DL.Ada_Type);
               Emit_Range_Check (Ctx, SV, "i64", SL.Ada_Type);
               Emit_Inst (Ctx.E, "store.i32 " & YV & ", " & To_String (YL.Reg) & ", align 4");
               Emit_Inst (Ctx.E, "store.i32 " & MV & ", " & To_String (ML.Reg) & ", align 4");
               Emit_Inst (Ctx.E, "store.i32 " & DV & ", " & To_String (DL.Reg) & ", align 4");
               Emit_Inst (Ctx.E, "store.i64 " & SV & ", " & To_String (SL.Reg) & ", align 8");
            end;
            return Res;
         elsif Name = """-""" or else Name = """+""" or else Name = """<"""
           or else Name = """<=""" or else Name = """>""" or else Name = """>="""
         then
            Handled := True;
            declare
               L  : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               R_V : constant Expr_Val := Emit_Expr (Ctx, Args (2));
               Out_R : Unbounded_String;
            begin
               Res.Kind := EK_Scalar;
               Res.Ada_Type := Res_Type;
               if Name = """-""" then
                  if Is_Fixed_Type (Res_Type) then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_sub_time",
                       To_String (L.Reg) & ", " & To_String (R_V.Reg), "i64", Out_R);
                  else
                     if Ctx.Enable_Checks then
                        declare
                           LoC : constant String := Next_Reg (Ctx.E, "cd1");
                           HiC : constant String := Next_Reg (Ctx.E, "cd2");
                           Bad : constant String := Next_Reg (Ctx.E, "cdb");
                        begin
                           Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i64 " & To_String (R_V.Reg) & ", -86400000000000");
                           Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i64 " & To_String (R_V.Reg) & ", 86400000000000");
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end;
                     end if;
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_sub_dur",
                       To_String (L.Reg) & ", " & To_String (R_V.Reg), "i64", Out_R);
                  end if;
                  Res.Pir_Type := To_Unbounded_String ("i64");
               elsif Name = """+""" then
                  declare
                     Dur_R : constant String := (if Is_Fixed_Type (L.Ada_Type) then To_String (L.Reg) else To_String (R_V.Reg));
                  begin
                     if Ctx.Enable_Checks then
                        declare
                           LoC : constant String := Next_Reg (Ctx.E, "cp1");
                           HiC : constant String := Next_Reg (Ctx.E, "cp2");
                           Bad : constant String := Next_Reg (Ctx.E, "cpb");
                        begin
                           Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i64 " & Dur_R & ", -86400000000000");
                           Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i64 " & Dur_R & ", 86400000000000");
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end;
                     end if;
                  end;
                  if Is_Fixed_Type (L.Ada_Type) then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_add",
                       To_String (R_V.Reg) & ", " & To_String (L.Reg), "i64", Out_R);
                  else
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_add",
                       To_String (L.Reg) & ", " & To_String (R_V.Reg), "i64", Out_R);
                  end if;
                  Res.Pir_Type := To_Unbounded_String ("i64");
               else
                  declare
                     CR : constant String := Next_Reg (Ctx.E, "cal");
                  begin
                     if Name = """<""" then
                        Emit_Inst (Ctx.E, CR & " = cmp_lt_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     elsif Name = """<=""" then
                        Emit_Inst (Ctx.E, CR & " = cmp_le_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     elsif Name = """>""" then
                        Emit_Inst (Ctx.E, CR & " = cmp_gt_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     else
                        Emit_Inst (Ctx.E, CR & " = cmp_ge_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     end if;
                     Out_R := To_Unbounded_String (CR);
                  end;
                  Res.Pir_Type := To_Unbounded_String ("i8");
               end if;
               Res.Reg := Out_R;
               return Res;
            end;
         end if;
      end if;
      end;
      return Res;
   end Try_Emit_RTL_Call;

   -- Compute the static link pointer to pass when calling Callee_SI from Ctx.Cur_Subp_Key
   function Compute_Call_SL (Ctx : in out Context; Callee_SI : Subp_Info) return String is
   begin
      if Callee_SI.Parent_Subp = Ctx.Cur_Subp_Key then
         if Length (Ctx.Cur_Frame_Reg) > 0 then
            return To_String (Ctx.Cur_Frame_Reg);
         else
            return "0";
         end if;
      end if;
      declare
         Cur_K : Unsigned_32 := Ctx.Cur_Subp_Key;
         SL    : Unbounded_String := Ctx.Cur_SL_Reg;
      begin
         if Length (SL) = 0 then
            return "0";
         end if;
         for Hop in 1 .. 16 loop
            exit when not Ctx.Subps.Contains (Cur_K);
            declare
               SI : constant Subp_Info := Ctx.Subps.Element (Cur_K);
               Parent_K : constant Unsigned_32 := SI.Parent_Subp;
            begin
               if Parent_K = Callee_SI.Parent_Subp then
                  return (if Length (SL) > 0 then To_String (SL) else "0");
               elsif Parent_K = 0 or else not Ctx.Subps.Contains (Parent_K) then
                  return "0";
               else
                  declare
                     PI  : constant Subp_Info := Ctx.Subps.Element (Parent_K);
                     FP  : constant String := Next_Reg (Ctx.E, "cslp");
                     NSL : constant String := Next_Reg (Ctx.E, "csl");
                  begin
                     if Length (PI.Frame_Type) = 0 then
                        return "0";
                     end if;
                     Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (PI.Frame_Type) & ", " & To_String (SL) & ", 0");
                     Emit_Inst (Ctx.E, NSL & " = load.ptr " & FP & ", align 8");
                     SL := To_Unbounded_String (NSL);
                     Cur_K := Parent_K;
                  end;
               end if;
            end;
         end loop;
      end;
      return "0";
   end Compute_Call_SL;

   function Emit_Subp_Call
     (Ctx       : in out Context;
      Call_Node : TREE;
      Is_Func   : Boolean) return Expr_Val
   is
      Name_Node : constant TREE := D (AS_NAME, Call_Node);
      Des_Node  : TREE := Name_Node;
      Def       : TREE;
      Norm_S    : TREE;
      Param_L   : SEQ_TYPE := MAKE;
      Res_Type  : TREE := CONST_VOID;
      Res       : Expr_Val;
   begin
      while not Is_Void_Tree (Des_Node) and then KIND (Des_Node) = DN_SELECTED loop
         Des_Node := D (AS_DESIGNATOR, Des_Node);
      end loop;
      if Is_Func then
         Res_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Call_Node));
      end if;
      if KIND (Call_Node) /= DN_NULLARY_CALL then
         Norm_S := D (SM_NORMALIZED_PARAM_S, Call_Node);
         if not Is_Void_Tree (Norm_S) then
            Param_L := LIST (Norm_S);
         else
            declare
               GA : constant TREE := D (AS_GENERAL_ASSOC_S, Call_Node);
            begin
               if not Is_Void_Tree (GA) then
                  Param_L := LIST (GA);
               end if;
            end;
         end if;
      end if;
      -- Resolve prefix of an attribute (supports T'BASE, Pkg.T, objects, components, indexed)
      if KIND (Des_Node) = DN_ATTRIBUTE then
         -- Attribute function call: POS, VAL, SUCC, PRED, IMAGE, VALUE
         declare
            Attr_Id : constant TREE := D (AS_USED_NAME_ID, Des_Node);
            AName   : constant String := Sym_Name (Attr_Id);
            Pfx_N   : constant TREE := D (AS_NAME, Des_Node);
            ET      : constant TREE := Res_Type;
            TI      : constant Type_Info := Get_Type_Info (Ctx, ET);
            Arg_N   : TREE := CONST_VOID;
            PL      : SEQ_TYPE := Param_L;

            function Attr_Pfx_Type (P : TREE) return TREE is
            begin
               if Is_Void_Tree (P) then
                  return CONST_VOID;
               elsif KIND (P) = DN_ATTRIBUTE then
                  if Sym_Name (D (AS_USED_NAME_ID, P)) = "BASE" then
                     return Base_Type_Spec (Attr_Pfx_Type (D (AS_NAME, P)));
                  end if;
                  return Subst_Tree (Ctx, D (SM_EXP_TYPE, P));
               elsif KIND (P) = DN_SELECTED then
                  declare
                     Des : constant TREE := D (AS_DESIGNATOR, P);
                     Df  : constant TREE := D (SM_DEFN, Des);
                  begin
                     if not Is_Void_Tree (Df) and then KIND (Df) in CLASS_TYPE_NAME then
                        return Subst_Tree (Ctx, D (SM_TYPE_SPEC, Df));
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
                  return CONST_VOID;
               end if;
            end Attr_Pfx_Type;

            procedure Ensure_Enum_Tables
              (Base_Enum : TREE;
               Ptrs_Sym  : out Unbounded_String;
               Count     : out Natural)
            is
            begin
               Ensure_Enum_Tables_Pkg (Ctx, Base_Enum, Ptrs_Sym, Count);
            end Ensure_Enum_Tables;
         begin
            if not IS_EMPTY (PL) then
               POP (PL, Arg_N);
            end if;
            if AName = "POS" then
               declare
                  V : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
               begin
                  Res.Kind := EK_Scalar; Res.Ada_Type := ET; Res.Pir_Type := TI.Pir_Type; Res.Align := TI.Align;
                  Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type)));
                  return Res;
               end;
            elsif AName = "VAL" then
               declare
                  PT : constant TREE := Attr_Pfx_Type (Pfx_N);
                  Chk_T : constant TREE := (if not Is_Void_Tree (PT) then Subst_Base_Type (Ctx, PT) else Subst_Base_Type (Ctx, ET));
                  V  : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
                  I64 : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i64");
                  Cv : constant String := Emit_Int_Convert (Ctx, I64, "i64", To_String (TI.Pir_Type));
               begin
                  -- Range check against base type bounds in i64 before narrowing!
                  Emit_Range_Check (Ctx, I64, "i64", Chk_T);
                  Res.Kind := EK_Scalar; Res.Ada_Type := ET; Res.Pir_Type := TI.Pir_Type; Res.Align := TI.Align;
                  Res.Reg := To_Unbounded_String (Cv);
                  return Res;
               end;
            elsif AName = "SUCC" or else AName = "PRED" then
               declare
                  PT_Sub : constant TREE := Attr_Pfx_Type (Pfx_N);
                  Chk_T  : constant TREE := (if not Is_Void_Tree (PT_Sub) then Subst_Base_Type (Ctx, PT_Sub) else Subst_Base_Type (Ctx, ET));
                  V    : constant Expr_Val := Emit_Expr (Ctx, Arg_N, ET);
                  PT   : constant String := To_String (TI.Pir_Type);
                  OutR : constant String := Next_Reg (Ctx.E, "sp");
                  Op   : constant String := (if AName = "SUCC" then "add" else "sub");
               begin
                  if Ctx.Enable_Checks then
                     declare
                        Ovf : constant String := Next_Reg (Ctx.E, "ov");
                     begin
                        Emit_Inst (Ctx.E, OutR & ", " & Ovf & " = " & Op & "_ovf_s." & PT & " " & To_String (V.Reg) & ", 1");
                        Emit_Check_Fail_If (Ctx, Ovf);
                     end;
                     Emit_Range_Check (Ctx, OutR, PT, Chk_T);
                  else
                     Emit_Inst (Ctx.E, OutR & " = " & Op & "." & PT & " " & To_String (V.Reg) & ", 1");
                  end if;
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (OutR);
                  Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                  return Res;
               end;
            elsif AName = "IMAGE" then
               declare
                  PT  : constant TREE := Attr_Pfx_Type (Pfx_N);
                  BT  : constant TREE := Subst_Base_Type (Ctx, PT);
                  V   : constant Expr_Val := Emit_Expr (Ctx, Arg_N, PT);
                  One : constant String := Next_Reg (Ctx.E, "imf");
               begin
                  Emit_Inst (Ctx.E, One & " = const.i32 1");
                  if Is_Integer_Type (BT) then
                     declare
                        V64 : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i64");
                        Buf, Len : Unbounded_String;
                     begin
                        Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", "32, 8", "ptr", Buf);
                        Emit_Call_Or_Invoke (Ctx, "@__ada_img_i64", V64 & ", " & To_String (Buf), "i32", Len, Nounwind => True);
                        Res.Kind := EK_Slice; Res.Reg := Buf; Res.Pir_Type := To_Unbounded_String ("ptr");
                        Res.Ada_Type := ET; Res.Align := 1; Res.NDims := 1;
                        Res.First_Regs (1) := To_Unbounded_String (One);
                        Res.Last_Regs (1)  := Len;
                        return Res;
                     end;
                  elsif Is_Character_Type (BT) then
                     declare
                        C8  : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i8");
                        Buf, Len : Unbounded_String;
                     begin
                        Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", "8, 8", "ptr", Buf);
                        Emit_Call_Or_Invoke (Ctx, "@__ada_img_char", C8 & ", " & To_String (Buf), "i32", Len, Nounwind => True);
                        Res.Kind := EK_Slice; Res.Reg := Buf; Res.Pir_Type := To_Unbounded_String ("ptr");
                        Res.Ada_Type := ET; Res.Align := 1; Res.NDims := 1;
                        Res.First_Regs (1) := To_Unbounded_String (One);
                        Res.Last_Regs (1)  := Len;
                        return Res;
                     end;
                  elsif not Is_Void_Tree (BT) and then KIND (BT) = DN_ENUMERATION then
                     declare
                        PS     : Unbounded_String;
                        Cnt    : Natural;
                        Idx64  : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i64");
                        PA     : constant String := Next_Reg (Ctx.E, "epa");
                        PE     : constant String := Next_Reg (Ctx.E, "epe");
                        SP     : constant String := Next_Reg (Ctx.E, "esp");
                        SL     : Unbounded_String;
                     begin
                        Ensure_Enum_Tables (BT, PS, Cnt);
                        pragma Unreferenced (Cnt);
                        Emit_Inst (Ctx.E, PA & " = addr " & To_String (PS));
                        Emit_Inst (Ctx.E, PE & " = elem_ptr ptr, " & PA & ", " & Idx64);
                        Emit_Inst (Ctx.E, SP & " = load.ptr " & PE & ", align 8");
                        Emit_Call_Or_Invoke (Ctx, "@__ada_cstr_len", SP, "i32", SL, Nounwind => True);
                        Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (SP);
                        Res.Pir_Type := To_Unbounded_String ("ptr");
                        Res.Ada_Type := ET; Res.Align := 1; Res.NDims := 1;
                        Res.First_Regs (1) := To_Unbounded_String (One);
                        Res.Last_Regs (1)  := SL;
                        return Res;
                     end;
                  else
                     declare
                        Empty : constant String := Next_Reg (Ctx.E, "ime");
                        Zero  : constant String := Next_Reg (Ctx.E, "imz");
                     begin
                        Emit_Inst (Ctx.E, Empty & " = const.ptr 0");
                        Emit_Inst (Ctx.E, Zero & " = const.i32 0");
                        Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (Empty);
                        Res.Pir_Type := To_Unbounded_String ("ptr");
                        Res.Ada_Type := ET; Res.Align := 1; Res.NDims := 1;
                        Res.First_Regs (1) := To_Unbounded_String (One);
                        Res.Last_Regs (1)  := To_Unbounded_String (Zero);
                        return Res;
                     end;
                  end if;
               end;
            elsif AName = "VALUE" then
               declare
                  PT  : constant TREE := Attr_Pfx_Type (Pfx_N);
                  BT  : constant TREE := Subst_Base_Type (Ctx, PT);
                  S   : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
                  SF, SL : Unbounded_String;
                  Df  : constant String := Next_Reg (Ctx.E, "vd");
                  Ln  : constant String := Next_Reg (Ctx.E, "vl");
                  Ps  : constant String := Next_Reg (Ctx.E, "vp");
                  Cl  : constant String := Next_Reg (Ctx.E, "vcl");
               begin
                  Get_Array_Dim_Bounds (Ctx, S, 1, SF, SL);
                  Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (SL) & ", " & To_String (SF));
                  Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                  Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                  Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                  if Is_Integer_Type (BT) then
                     declare
                        RV : Unbounded_String;
                        Cv : Unbounded_String;
                     begin
                        Emit_Call_Or_Invoke (Ctx, "@__ada_val_i64", To_String (S.Reg) & ", " & Cl, "i64", RV);
                        Emit_Range_Check (Ctx, To_String (RV), "i64", BT);
                        Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i64", To_String (TI.Pir_Type)));
                        Res.Kind := EK_Scalar; Res.Reg := Cv;
                        Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  elsif Is_Character_Type (BT) then
                     declare
                        RV : Unbounded_String;
                     begin
                        Emit_Call_Or_Invoke (Ctx, "@__ada_val_char", To_String (S.Reg) & ", " & Cl, "i8", RV);
                        Res.Kind := EK_Scalar; Res.Reg := RV;
                        Res.Pir_Type := To_Unbounded_String ("i8"); Res.Ada_Type := ET; Res.Align := 1;
                        return Res;
                     end;
                  elsif not Is_Void_Tree (BT) and then KIND (BT) = DN_ENUMERATION then
                     declare
                        PS     : Unbounded_String;
                        Cnt    : Natural;
                        TA     : constant String := Next_Reg (Ctx.E, "eta");
                        RV     : Unbounded_String;
                        Cv     : Unbounded_String;
                     begin
                        Ensure_Enum_Tables (BT, PS, Cnt);
                        Emit_Inst (Ctx.E, TA & " = addr " & To_String (PS));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_val_enum",
                          To_String (S.Reg) & ", " & Cl & ", " & TA & ", " & Img (Cnt), "i32", RV);
                        Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", To_String (TI.Pir_Type)));
                        Res.Kind := EK_Scalar; Res.Reg := Cv;
                        Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  else
                     declare
                        R : constant String := Next_Reg (Ctx.E, "vz");
                     begin
                        Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " 0");
                        Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                        Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  end if;
               end;
            else
               declare
                  R : constant String := Next_Reg (Ctx.E, "af");
                  PT : constant String := (if To_String (TI.Pir_Type) = "void" then "i32" else To_String (TI.Pir_Type));
               begin
                  Emit_Inst (Ctx.E, R & " = const." & PT & " 0");
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := To_Unbounded_String (PT); Res.Ada_Type := ET; Res.Align := TI.Align;
                  return Res;
               end;
            end if;
         end;
      end if;
      Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des_Node)));

      -- 0. Unwrap derived subprograms (DN_DERIVED_SUBPROG) and renamed subprograms (DN_RENAMES_UNIT)
      for Unwrap_Step in 1 .. 16 loop
         exit when Is_Void_Tree (Def) or else KIND (Def) not in CLASS_SUBPROG_NAME;
         declare
            UD : TREE := D (SM_UNIT_DESC, Def);
         begin
            if Is_Void_Tree (UD) and then Ctx.Subp_Renames.Contains (Tree_Key (Def)) then
               UD := Ctx.Subp_Renames.Element (Tree_Key (Def));
            end if;
            exit when Is_Void_Tree (UD);
            if KIND (UD) = DN_DERIVED_SUBPROG then
               Def := Canonical_Def (Subst_Tree (Ctx, D (SM_DERIVABLE, UD)));
            elsif KIND (UD) = DN_RENAMES_UNIT then
               declare
                  RN : constant TREE := D (AS_NAME, UD);
               begin
                  exit when Is_Void_Tree (RN);
                  if KIND (RN) = DN_ATTRIBUTE then
                     -- Renamed attribute function (e.g. function Next renames T'Succ, T'Image, T'Value)
                     declare
                        Attr_Id   : constant TREE := D (AS_USED_NAME_ID, RN);
                        AName     : constant String := Sym_Name (Attr_Id);
                        Pfx_N     : constant TREE := D (AS_NAME, RN);
                        TI        : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
                        Arg_N     : TREE := CONST_VOID;
                        PL        : SEQ_TYPE := Param_L;
                     begin
                        if not IS_EMPTY (PL) then POP (PL, Arg_N); end if;
                        if AName = "POS" then
                           declare
                              V : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
                           begin
                              Res.Kind := EK_Scalar; Res.Ada_Type := Res_Type;
                              Res.Pir_Type := TI.Pir_Type; Res.Align := TI.Align;
                              Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type)));
                              return Res;
                           end;
                        elsif AName = "VAL" then
                           declare
                              V  : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
                              Cv : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type));
                           begin
                              Emit_Range_Check (Ctx, Cv, To_String (TI.Pir_Type), Base_Type_Spec (Res_Type));
                              Res.Kind := EK_Scalar; Res.Ada_Type := Res_Type;
                              Res.Pir_Type := TI.Pir_Type; Res.Align := TI.Align;
                              Res.Reg := To_Unbounded_String (Cv);
                              return Res;
                           end;
                        elsif AName = "SUCC" or else AName = "PRED" then
                           declare
                              V    : constant Expr_Val := Emit_Expr (Ctx, Arg_N, Res_Type);
                              PT   : constant String := To_String (TI.Pir_Type);
                              VR   : constant String := Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), PT);
                              OutR : constant String := Next_Reg (Ctx.E, "sp");
                              Op   : constant String := (if AName = "SUCC" then "add" else "sub");
                           begin
                              Emit_Inst (Ctx.E, OutR & " = " & Op & "." & PT & " " & VR & ", 1");
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (OutR);
                              Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := TI.Align;
                              return Res;
                           end;
                        elsif AName = "IMAGE" or else AName = "VALUE" then
                           declare
                              Pfx_Base : constant TREE :=
                                (if not Is_Void_Tree (Pfx_N) and then KIND (Pfx_N) = DN_ATTRIBUTE
                                 then D (AS_NAME, Pfx_N) else Pfx_N);
                              Pfx_Def  : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Pfx_Base)));
                              Pfx_TS   : constant TREE :=
                                (if Is_Void_Tree (Pfx_Def) then CONST_VOID
                                 elsif KIND (Pfx_Def) in CLASS_TYPE_NAME
                                 then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Pfx_Def)))
                                 else Resolve_Type_Spec (Pfx_Def));
                              Pfx_BT   : constant TREE := Base_Type_Spec (Pfx_TS);
                           begin
                              if AName = "IMAGE" then
                                 declare
                                    V      : constant Expr_Val := Emit_Expr (Ctx, Arg_N, Pfx_TS);
                                    One_R  : constant String := Next_Reg (Ctx.E, "im1");
                                 begin
                                    Emit_Inst (Ctx.E, One_R & " = const.i32 1");
                                    if Is_Integer_Type (Pfx_BT) then
                                       declare
                                          V64 : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i64");
                                          Buf, Len : Unbounded_String;
                                       begin
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", "32, 8", "ptr", Buf);
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_img_i64", V64 & ", " & To_String (Buf), "i32", Len, Nounwind => True);
                                          Res.Kind := EK_Slice; Res.Reg := Buf; Res.Pir_Type := To_Unbounded_String ("ptr");
                                          Res.Ada_Type := Res_Type; Res.Align := 1; Res.NDims := 1;
                                          Res.First_Regs (1) := To_Unbounded_String (One_R);
                                          Res.Last_Regs (1)  := Len;
                                          return Res;
                                       end;
                                    elsif Is_Character_Type (Pfx_BT) then
                                       declare
                                          C8 : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i8");
                                          Buf, Len : Unbounded_String;
                                       begin
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", "8, 8", "ptr", Buf);
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_img_char", C8 & ", " & To_String (Buf), "i32", Len, Nounwind => True);
                                          Res.Kind := EK_Slice; Res.Reg := Buf; Res.Pir_Type := To_Unbounded_String ("ptr");
                                          Res.Ada_Type := Res_Type; Res.Align := 1; Res.NDims := 1;
                                          Res.First_Regs (1) := To_Unbounded_String (One_R);
                                          Res.Last_Regs (1)  := Len;
                                          return Res;
                                       end;
                                    elsif not Is_Void_Tree (Pfx_BT) and then KIND (Pfx_BT) = DN_ENUMERATION then
                                       declare
                                          PS    : Unbounded_String;
                                          Cnt   : Natural;
                                          Idx64 : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i64");
                                          PA    : constant String := Next_Reg (Ctx.E, "epa");
                                          PE    : constant String := Next_Reg (Ctx.E, "epe");
                                          SP    : constant String := Next_Reg (Ctx.E, "esp");
                                          SL    : Unbounded_String;
                                       begin
                                          Ensure_Enum_Tables_Pkg (Ctx, Pfx_BT, PS, Cnt);
                                          pragma Unreferenced (Cnt);
                                          Emit_Inst (Ctx.E, PA & " = addr " & To_String (PS));
                                          Emit_Inst (Ctx.E, PE & " = elem_ptr ptr, " & PA & ", " & Idx64);
                                          Emit_Inst (Ctx.E, SP & " = load.ptr " & PE & ", align 8");
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_cstr_len", SP, "i32", SL, Nounwind => True);
                                          Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (SP);
                                          Res.Pir_Type := To_Unbounded_String ("ptr");
                                          Res.Ada_Type := Res_Type; Res.Align := 1; Res.NDims := 1;
                                          Res.First_Regs (1) := To_Unbounded_String (One_R);
                                          Res.Last_Regs (1)  := SL;
                                          return Res;
                                       end;
                                    end if;
                                 end;
                              else -- VALUE
                                 declare
                                    SV : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
                                    F, L : Unbounded_String;
                                    Df : constant String := Next_Reg (Ctx.E, "vdf");
                                    Ln : constant String := Next_Reg (Ctx.E, "vln");
                                    Ps : constant String := Next_Reg (Ctx.E, "vps");
                                    Cl : constant String := Next_Reg (Ctx.E, "vcl");
                                    RV : Unbounded_String;
                                    Cv : Unbounded_String;
                                 begin
                                    Get_Array_Dim_Bounds (Ctx, SV, 1, F, L);
                                    Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L) & ", " & To_String (F));
                                    Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                                    Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                                    Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                                    if Is_Character_Type (Pfx_BT) then
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_val_char", To_String (SV.Reg) & ", " & Cl, "i8", RV);
                                       Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i8", To_String (TI.Pir_Type)));
                                    elsif not Is_Void_Tree (Pfx_BT) and then KIND (Pfx_BT) = DN_ENUMERATION then
                                       declare
                                          PS  : Unbounded_String;
                                          Cnt : Natural;
                                          TA  : constant String := Next_Reg (Ctx.E, "evt");
                                       begin
                                          Ensure_Enum_Tables_Pkg (Ctx, Pfx_BT, PS, Cnt);
                                          Emit_Inst (Ctx.E, TA & " = addr " & To_String (PS));
                                          Emit_Call_Or_Invoke (Ctx, "@__ada_val_enum",
                                            To_String (SV.Reg) & ", " & Cl & ", " & TA & ", " & Img (Cnt), "i32", RV);
                                          Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", To_String (TI.Pir_Type)));
                                       end;
                                    else
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_val_i64",
                                         To_String (SV.Reg) & ", " & Cl, "i64", RV);
                                       Emit_Range_Check (Ctx, To_String (RV), "i64", Pfx_BT);
                                       Cv := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i64", To_String (TI.Pir_Type)));
                                    end if;
                                    Emit_Range_Check (Ctx, To_String (Cv), To_String (TI.Pir_Type), Res_Type);
                                    Res.Kind := EK_Scalar; Res.Reg := Cv;
                                    Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := TI.Align;
                                    return Res;
                                 end;
                              end if;
                           end;
                        end if;
                        exit;
                     end;
                  else
                     Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (RN)));
                  end if;
               end;
            else
               exit;
            end if;
         end;
      end loop;

      -- 0b. Renamed enumeration/character literal called as parameterless function!
      if not Is_Void_Tree (Def) and then (KIND (Def) = DN_ENUMERATION_ID or else KIND (Def) = DN_CHARACTER_ID) then
         declare
            TI  : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
            Pos : constant Integer := DI (SM_POS, Def);
            R   : constant String := Next_Reg (Ctx.E, "ren");
         begin
            Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Pos));
            Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
            Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := TI.Align;
            return Res;
         end;
      end if;

      -- 1. Built-in operator
      if not Is_Void_Tree (Def) and then KIND (Def) = DN_BLTN_OPERATOR_ID then
         -- LRM 4.10 (c4a010a): fold exact static universal_real expressions/comparisons from semphase SM_VALUE
         declare
            SV : constant TREE := Safe_SM_Value (Call_Node);
         begin
            if Has_Static_Val (SV) and then not IS_EMPTY (Param_L) then
               declare
                  A1   : constant TREE := HEAD (Param_L);
                  A1_T : constant TREE :=
                    (if not Is_Void_Tree (A1) and then KIND (A1) in CLASS_EXP
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, A1)))
                     else CONST_VOID);
               begin
                  if not Is_Void_Tree (A1_T) and then KIND (A1_T) = DN_UNIVERSAL_REAL then
                     declare
                        RTI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
                        R   : constant String := Next_Reg (Ctx.E, "urf");
                     begin
                        if Is_Float_Type (Res_Type) then
                           Emit_Inst (Ctx.E, R & " = const." & To_String (RTI.Pir_Type) & " " & Format_Float64 (Eval_Float64 (SV)));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                           return Res;
                        elsif Is_Discrete_Type (Res_Type) then
                           Emit_Inst (Ctx.E, R & " = const." & To_String (RTI.Pir_Type) & " " & Img (Long_Long_Integer (Eval_Int64 (SV))));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                           return Res;
                        end if;
                     end;
                  end if;
               end;
            end if;
         end;
         return Emit_Builtin_Op (Ctx, Def, Param_L, Res_Type);
      end if;

      -- Guard non-subprogram targets (e.g. task entries, unhandled renames)
      if Is_Void_Tree (Def) or else KIND (Def) not in CLASS_SUBPROG_NAME then
         if Is_Func and then not Is_Void_Tree (Res_Type) then
            declare
               TI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
               R  : constant String := Next_Reg (Ctx.E, "ns");
            begin
               if TI.Kind = TK_Scalar then
                  Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " 0");
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := TI.Align;
               end if;
            end;
         end if;
         return Res;
      end if;

      -- 2. Implicit /= on user-defined "=" (unwrap renamings/derived/builtin "=" properly!)
      if KIND (Def) in CLASS_SUBPROG_NAME then
         declare
            UD : constant TREE := D (SM_UNIT_DESC, Def);
         begin
            if not Is_Void_Tree (UD) and then KIND (UD) = DN_IMPLICIT_NOT_EQ then
               declare
                  Eq_Def  : TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_EQUAL, UD)));
                  Eq_Call : Expr_Val;
                  Inv     : constant String := Next_Reg (Ctx.E, "ineq");
               begin
                  -- Unwrap any renaming / derived chain on Eq_Def
                  for UStep in 1 .. 16 loop
                     exit when Is_Void_Tree (Eq_Def) or else KIND (Eq_Def) not in CLASS_SUBPROG_NAME;
                     declare
                        EUD : TREE := D (SM_UNIT_DESC, Eq_Def);
                     begin
                        if Is_Void_Tree (EUD) and then Ctx.Subp_Renames.Contains (Tree_Key (Eq_Def)) then
                           EUD := Ctx.Subp_Renames.Element (Tree_Key (Eq_Def));
                        end if;
                        exit when Is_Void_Tree (EUD);
                        if KIND (EUD) = DN_DERIVED_SUBPROG then
                           Eq_Def := Canonical_Def (Subst_Tree (Ctx, D (SM_DERIVABLE, EUD)));
                        elsif KIND (EUD) = DN_RENAMES_UNIT then
                           declare
                              ERN : constant TREE := D (AS_NAME, EUD);
                           begin
                              exit when Is_Void_Tree (ERN);
                              Eq_Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (ERN)));
                           end;
                        else
                           exit;
                        end if;
                     end;
                  end loop;
                  if not Is_Void_Tree (Eq_Def) and then KIND (Eq_Def) = DN_BLTN_OPERATOR_ID then
                     Eq_Call := Emit_Builtin_Op (Ctx, Eq_Def, Param_L, Res_Type);
                     Emit_Inst (Ctx.E, Inv & " = xor.i8 " & To_String (Eq_Call.Reg) & ", 1");
                     Eq_Call.Reg := To_Unbounded_String (Inv);
                     return Eq_Call;
                  elsif not Is_Void_Tree (Eq_Def) and then KIND (Eq_Def) in CLASS_SUBPROG_NAME then
                     -- Replace Def with Eq_Def and emit normal call, then invert!
                     declare
                        SI    : Subp_Info;
                        Key   : constant Unsigned_32 := Tree_Key (Eq_Def);
                        Args  : Unbounded_String;
                        RV    : Unbounded_String;
                        PL    : SEQ_TYPE := Param_L;
                        A1, A2 : TREE;
                        V1, V2 : Expr_Val;
                     begin
                        if Ctx.Subps.Contains (Key) then
                           SI := Ctx.Subps.Element (Key);
                        else
                           declare
                              Spec : constant TREE := D (SM_SPEC, Eq_Def);
                           begin
                              SI.Spec_Node   := Spec;
                              SI.Pir_Name    := To_Unbounded_String (Mangle_Subp_Name (Ctx, Eq_Def, Spec, Is_Subunit => Ctx.In_Subunit));
                              SI.Is_Function := True;
                              SI.Is_External := True;
                              SI.Ret_Type    := Res_Type;
                              Ctx.Subps.Insert (Key, SI);
                              Ensure_Extern_Subp (Ctx, Eq_Def, SI);
                           end;
                        end if;
                        POP (PL, A1); POP (PL, A2);
                        -- Determine formal parameter types for proper composite/uncon-array marshalling
                        declare
                           PS_N : constant TREE := (if not Is_Void_Tree (SI.Spec_Node) then D (AS_PARAM_S, SI.Spec_Node) else CONST_VOID);
                           FT1, FT2 : TREE := CONST_VOID;
                           Idx_P : Natural := 0;
                        begin
                           if not Is_Void_Tree (PS_N) then
                              declare
                                 FPL : SEQ_TYPE := LIST (PS_N);
                                 FP  : TREE;
                              begin
                                 while not IS_EMPTY (FPL) loop
                                    POP (FPL, FP);
                                    declare
                                       FNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FP));
                                       FID : TREE;
                                    begin
                                       while not IS_EMPTY (FNS) loop
                                          POP (FNS, FID);
                                          Idx_P := Idx_P + 1;
                                          if Idx_P = 1 then FT1 := Subst_Tree (Ctx, D (SM_OBJ_TYPE, FID));
                                          elsif Idx_P = 2 then FT2 := Subst_Tree (Ctx, D (SM_OBJ_TYPE, FID));
                                          end if;
                                       end loop;
                                    end;
                                 end loop;
                              end;
                           end if;
                           if SI.Needs_SL then
                              Append (Args, Compute_Call_SL (Ctx, SI) & ", ");
                           end if;
                           declare
                              procedure Append_Eq_Arg (Act : TREE; FT : TREE; Need_Comma : Boolean) is
                                 FTI : constant Type_Info := (if not Is_Void_Tree (FT) then Get_Type_Info (Ctx, FT) else (others => <>));
                                 AV  : Expr_Val;
                              begin
                                 if Need_Comma then Append (Args, ", "); end if;
                                 if not Is_Void_Tree (FT)
                                   and then (FTI.Kind = TK_Unconstrained_Array
                                             or else (FTI.Kind = TK_Constrained_Array and then FTI.Is_Dynamic))
                                 then
                                    AV := Emit_Expr (Ctx, Act);
                                    Append (Args, To_String (AV.Reg));
                                    for D_I in 1 .. Natural'Max (1, FTI.NDims) loop
                                       declare
                                          F_R, L_R : Unbounded_String;
                                       begin
                                          Get_Array_Dim_Bounds (Ctx, AV, D_I, F_R, L_R);
                                          Append (Args, ", " & To_String (F_R) & ", " & To_String (L_R));
                                       end;
                                    end loop;
                                 else
                                    AV := Emit_Expr (Ctx, Act, FT);
                                    if not Is_Void_Tree (FT) and then FTI.Kind = TK_Scalar then
                                       declare
                                          VR : constant String := Emit_Scalar_Coerce (Ctx, To_String (AV.Reg), To_String (AV.Pir_Type), To_String (FTI.Pir_Type));
                                       begin
                                          Emit_Range_Check (Ctx, VR, To_String (FTI.Pir_Type), FT);
                                          Emit_Discriminant_Check (Ctx, VR, FT, Is_Access => True);
                                          Append (Args, VR);
                                       end;
                                    else
                                       Append (Args, To_String (AV.Reg));
                                    end if;
                                 end if;
                              end Append_Eq_Arg;
                           begin
                              Append_Eq_Arg (A1, FT1, False);
                              Append_Eq_Arg (A2, FT2, True);
                           end;
                        end;
                        Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args), "i8", RV);
                        Emit_Inst (Ctx.E, Inv & " = xor.i8 " & To_String (RV) & ", 1");
                        Eq_Call.Kind := EK_Scalar;
                        Eq_Call.Reg := To_Unbounded_String (Inv);
                        Eq_Call.Pir_Type := To_Unbounded_String ("i8");
                        Eq_Call.Ada_Type := Res_Type;
                        return Eq_Call;
                     end;
                  end if;
               end;
            end if;
         end;
      end if;

      -- 3. Predefined RTL calls (Text_IO, Calendar, Unchecked_*)
      declare
         RTL_Handled : Boolean := False;
      begin
         Res := Try_Emit_RTL_Call (Ctx, Def, Param_L, Res_Type, RTL_Handled);
         if RTL_Handled then
            return Res;
         end if;
      end;

      -- 4. User subprogram call (local, nested, generic instance, or external package)
      declare
         Key : constant Unsigned_32 := Tree_Key (Def);
         SI  : Subp_Info;
      begin
         if Ctx.Subps.Contains (Key) then
            SI := Ctx.Subps.Element (Key);
            if not Is_Void_Tree (D (SM_INTERFACE, Def)) then
               SI.Is_C_Interface := Is_C_Interfaced (Def);
               if SI.Is_C_Interface then
                  SI.Needs_SL := False;
               end if;
               Ensure_Extern_Subp (Ctx, Def, SI);
            end if;
         else
            -- External subprogram from a with'd package spec or ancestor subunit
            declare
               Spec : constant TREE := D (SM_SPEC, Def);
            begin
               SI.Spec_Node      := Spec;
               SI.Pir_Name       := To_Unbounded_String (Mangle_Subp_Name (Ctx, Def, Spec, Is_Subunit => Ctx.In_Subunit));
               SI.Is_Function    := KIND (Spec) = DN_FUNCTION_SPEC;
               SI.Is_External    := True;
               SI.Is_C_Interface := Is_C_Interfaced (Def);
               if SI.Is_C_Interface then
                  SI.Needs_SL := False;
               end if;
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
               Ensure_Extern_Subp (Ctx, Def, SI);
            end;
         end if;

         declare
            Args_Buf : Unbounded_String;
            First_A  : Boolean := True;
            Ret_Buf  : Unbounded_String;

            procedure Add_Arg (S : String) is
            begin
               if not First_A then
                  Append (Args_Buf, ", ");
               end if;
               First_A := False;
               Append (Args_Buf, S);
            end Add_Arg;
         begin
            if SI.Needs_SL then
               Add_Arg (Compute_Call_SL (Ctx, SI));
            end if;
            if SI.Ret_Uncon_Arr then
               declare
                  RTI     : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, RTI.NDims);
                  Tmp     : constant String := Next_Reg (Ctx.E, "retdesc");
               begin
                  Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & Desc_Ty & ", align 8");
                  Ret_Buf := To_Unbounded_String (Tmp);
                  Add_Arg (Tmp);
               end;
            elsif SI.Ret_Composite then
               declare
                  RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  Tmp : constant String := Next_Reg (Ctx.E, "retbuf");
               begin
                  Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (RTI.Pir_Type) & ", align " & Img (RTI.Align));
                  Ret_Buf := To_Unbounded_String (Tmp);
                  Add_Arg (Tmp);
               end;
            end if;

            -- Walk formal parameters in lockstep with actuals
            declare
               type Post_Acc_Check is record
                  Act_Ptr  : Unbounded_String;
                  Act_Type : TREE;
               end record;
               package PAC_Vecs is new Ada.Containers.Vectors (Natural, Post_Acc_Check);
               Post_Checks : PAC_Vecs.Vector;
               type Post_Conv_Out is record
                  Tmp_Ptr   : Unbounded_String;
                  Dst_Ptr   : Unbounded_String;
                  Src_Pir   : Unbounded_String;
                  Dst_Pir   : Unbounded_String;
                  Src_Align : Natural := 4;
                  Dst_Align : Natural := 4;
                  Conv_Type : TREE := CONST_VOID;
                  Src_Type  : TREE := CONST_VOID;
                  Dst_Type  : TREE := CONST_VOID;
               end record;
               package PCO_Vecs is new Ada.Containers.Vectors (Natural, Post_Conv_Out);
               Conv_Outs : PCO_Vecs.Vector;
            begin
            if not Is_Void_Tree (SI.Spec_Node) then
               declare
                  PS : constant TREE := D (AS_PARAM_S, SI.Spec_Node);
                  FL : SEQ_TYPE := (if Is_Void_Tree (PS) then MAKE else LIST (PS));
                  AL : SEQ_TYPE := Param_L;
                  FP : TREE;
               begin
                  while not IS_EMPTY (FL) loop
                     POP (FL, FP);
                     declare
                        Mode : constant NODE_NAME := KIND (FP);
                        FNS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FP));
                        FID  : TREE;
                        Act  : TREE;
                     begin
                        while not IS_EMPTY (FNS) loop
                           POP (FNS, FID);
                           if not IS_EMPTY (AL) then
                              POP (AL, Act);
                           else
                              Act := D (SM_INIT_EXP, FID);
                           end if;
                           declare
                              FT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, FID));
                              FTI : constant Type_Info := Get_Type_Info (Ctx, FT);
                           begin
                              if FTI.Kind = TK_Unconstrained_Array
                                or else (FTI.Kind = TK_Constrained_Array and then FTI.Is_Dynamic)
                              then
                                 declare
                                    AV : Expr_Val;
                                 begin
                                    if Mode = DN_IN then
                                       AV := Emit_Expr (Ctx, Act, FT);
                                    else
                                       AV := Emit_LValue (Ctx, Act);
                                    end if;
                                    if FTI.Kind = TK_Constrained_Array then
                                       -- LRM 6.4.1(5) (cc1311b): constrained array formal requires exact bounds match
                                       declare
                                          Tgt : Expr_Val;
                                          ND_A : constant Natural := Natural'Max (1, Natural'Min (FTI.NDims, 4));
                                          Act_U : TREE := Act;
                                       begin
                                          while not Is_Void_Tree (Act_U) and then KIND (Act_U) = DN_PARENTHESIZED loop
                                             Act_U := D (AS_EXP, Act_U);
                                          end loop;
                                          Tgt.Kind := EK_Slice; Tgt.Ada_Type := FT; Tgt.NDims := ND_A;
                                          for D_Idx in 1 .. ND_A loop
                                             Emit_Index_Bounds (Ctx, FTI.Dims (D_Idx).Idx_Type, Tgt.First_Regs (D_Idx), Tgt.Last_Regs (D_Idx));
                                          end loop;
                                          if not Is_Void_Tree (Act_U) and then KIND (Act_U) in DN_STRING_LITERAL | DN_AGGREGATE then
                                             Emit_Array_Length_Check (Ctx, Tgt, AV, ND_A);
                                          elsif Ctx.Enable_Checks then
                                             for D_Idx in 1 .. ND_A loop
                                                declare
                                                   AF, AL : Unbounded_String;
                                                   C1  : constant String := Next_Reg (Ctx.E, "pbf1");
                                                   C2  : constant String := Next_Reg (Ctx.E, "pbf2");
                                                   Bad : constant String := Next_Reg (Ctx.E, "pbfb");
                                                begin
                                                   Get_Array_Dim_Bounds (Ctx, AV, D_Idx, AF, AL);
                                                   Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (AF) & ", " & To_String (Tgt.First_Regs (D_Idx)));
                                                   Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (AL) & ", " & To_String (Tgt.Last_Regs (D_Idx)));
                                                   Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                                   Emit_Check_Fail_If (Ctx, Bad);
                                                end;
                                             end loop;
                                          end if;
                                          Add_Arg (To_String (AV.Reg));
                                          for D_Idx in 1 .. ND_A loop
                                             Add_Arg (To_String (Tgt.First_Regs (D_Idx)));
                                             Add_Arg (To_String (Tgt.Last_Regs (D_Idx)));
                                          end loop;
                                       end;
                                    else
                                       Add_Arg (To_String (AV.Reg));
                                       for D_Idx in 1 .. FTI.NDims loop
                                          declare
                                             F_R, L_R : Unbounded_String;
                                          begin
                                             Get_Array_Dim_Bounds (Ctx, AV, D_Idx, F_R, L_R);
                                             Add_Arg (To_String (F_R));
                                             Add_Arg (To_String (L_R));
                                          end;
                                       end loop;
                                    end if;
                                 end;
                              elsif FTI.Kind = TK_Record or else FTI.Kind = TK_Constrained_Array then
                                 declare
                                    AV : Expr_Val;
                                 begin
                                    if Mode = DN_IN then
                                       AV := Emit_Expr (Ctx, Act, FT);
                                    else
                                       AV := Emit_LValue (Ctx, Act);
                                    end if;
                                    if FTI.Kind = TK_Record then
                                       Emit_Discriminant_Check (Ctx, To_String (AV.Reg), FT, Is_Access => False);
                                    else
                                       declare
                                          Tgt : Expr_Val;
                                          ND_A : constant Natural := Natural'Max (1, Natural'Min (FTI.NDims, 4));
                                          Act_U : TREE := Act;
                                       begin
                                          while not Is_Void_Tree (Act_U) and then KIND (Act_U) = DN_PARENTHESIZED loop
                                             Act_U := D (AS_EXP, Act_U);
                                          end loop;
                                          Tgt.Kind := EK_Slice; Tgt.Ada_Type := FT; Tgt.NDims := ND_A;
                                          for D_Idx in 1 .. ND_A loop
                                             Emit_Index_Bounds (Ctx, FTI.Dims (D_Idx).Idx_Type, Tgt.First_Regs (D_Idx), Tgt.Last_Regs (D_Idx));
                                          end loop;
                                          if not Is_Void_Tree (Act_U) and then KIND (Act_U) in DN_STRING_LITERAL | DN_AGGREGATE then
                                             Emit_Array_Length_Check (Ctx, Tgt, AV, ND_A);
                                          elsif Ctx.Enable_Checks then
                                             for D_Idx in 1 .. ND_A loop
                                                declare
                                                   AF, AL : Unbounded_String;
                                                   C1  : constant String := Next_Reg (Ctx.E, "pbf1");
                                                   C2  : constant String := Next_Reg (Ctx.E, "pbf2");
                                                   Bad : constant String := Next_Reg (Ctx.E, "pbfb");
                                                begin
                                                   Get_Array_Dim_Bounds (Ctx, AV, D_Idx, AF, AL);
                                                   Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (AF) & ", " & To_String (Tgt.First_Regs (D_Idx)));
                                                   Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (AL) & ", " & To_String (Tgt.Last_Regs (D_Idx)));
                                                   Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                                   Emit_Check_Fail_If (Ctx, Bad);
                                                end;
                                             end loop;
                                          end if;
                                       end;
                                    end if;
                                    Add_Arg (To_String (AV.Reg));
                                    if Needs_Constrained_Param (Ctx, FT, Mode) then
                                       Add_Arg (Emit_Actual_Constrained (Ctx, Act));
                                    end if;
                                 end;
                              elsif Mode = DN_OUT or else Mode = DN_IN_OUT then
                                 if KIND (Act) = DN_CONVERSION then
                                    -- LRM 6.4.1(3): view conversion on scalar/access OUT or IN OUT actual
                                    declare
                                       Inner_N  : constant TREE := D (AS_EXP, Act);
                                       Inner_LV : constant Expr_Val := Emit_LValue (Ctx, Inner_N);
                                       Conv_T   : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Act));
                                       Tmp_Slot : constant String := Next_Reg (Ctx.E, "vct");
                                       ITI      : constant Type_Info := Get_Type_Info (Ctx, Inner_LV.Ada_Type);
                                    begin
                                       Emit_Entry_Alloca (Ctx.E, Tmp_Slot & " = alloca " & To_String (FTI.Pir_Type) & ", align " & Img (Natural'Max (1, FTI.Align)));
                                       if Mode = DN_IN_OUT then
                                          declare
                                             Ld  : constant String := Next_Reg (Ctx.E, "vci");
                                          begin
                                             Emit_Inst (Ctx.E, Ld & " = load." & To_String (ITI.Pir_Type) & " " & To_String (Inner_LV.Reg) & ", align " & Img (Natural'Max (1, ITI.Align)));
                                             declare
                                                C_Reg : constant String := Emit_Typed_Scalar_Convert (Ctx, Ld, Inner_LV.Ada_Type, To_String (ITI.Pir_Type), Conv_T, To_String (FTI.Pir_Type));
                                             begin
                                                Emit_Range_Check (Ctx, C_Reg, To_String (FTI.Pir_Type), Conv_T);
                                                Emit_Range_Check (Ctx, C_Reg, To_String (FTI.Pir_Type), FT);
                                                Emit_Discriminant_Check (Ctx, C_Reg, FT, Is_Access => True);
                                                Emit_Inst (Ctx.E, "store." & To_String (FTI.Pir_Type) & " " & C_Reg & ", " & Tmp_Slot & ", align " & Img (Natural'Max (1, FTI.Align)));
                                             end;
                                          end;
                                       else
                                          Emit_Inst (Ctx.E, "memset " & Tmp_Slot & ", 0, " & Img (Natural'Max (1, FTI.Byte_Size)) & ", align 1");
                                       end if;
                                       Conv_Outs.Append ((
                                         Tmp_Ptr   => To_Unbounded_String (Tmp_Slot),
                                         Dst_Ptr   => Inner_LV.Reg,
                                         Src_Pir   => FTI.Pir_Type,
                                         Dst_Pir   => ITI.Pir_Type,
                                         Src_Align => Natural'Max (1, FTI.Align),
                                         Dst_Align => Natural'Max (1, ITI.Align),
                                         Conv_Type => Conv_T,
                                         Src_Type  => FT,
                                         Dst_Type  => Inner_LV.Ada_Type));
                                       Add_Arg (Tmp_Slot);
                                    end;
                                 else
                                    -- LRM 6.4.1(4): scalar/access OUT or IN OUT copy-in / copy-out range & constraint checks
                                    declare
                                       LV  : constant Expr_Val := Emit_LValue (Ctx, Act);
                                       ITI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
                                       Tmp_Slot : constant String := Next_Reg (Ctx.E, "iot");
                                    begin
                                       Emit_Entry_Alloca (Ctx.E, Tmp_Slot & " = alloca " & To_String (FTI.Pir_Type) & ", align " & Img (Natural'Max (1, FTI.Align)));
                                       if Mode = DN_IN_OUT then
                                          declare
                                             Ld : constant String := Next_Reg (Ctx.E, "ioi");
                                          begin
                                             Emit_Inst (Ctx.E, Ld & " = load." & To_String (ITI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (Natural'Max (1, ITI.Align)));
                                             declare
                                                C_Reg : constant String := Emit_Typed_Scalar_Convert (Ctx, Ld, LV.Ada_Type, To_String (ITI.Pir_Type), FT, To_String (FTI.Pir_Type));
                                             begin
                                                Emit_Range_Check (Ctx, C_Reg, To_String (FTI.Pir_Type), FT);
                                                Emit_Discriminant_Check (Ctx, C_Reg, FT, Is_Access => True);
                                                Emit_Inst (Ctx.E, "store." & To_String (FTI.Pir_Type) & " " & C_Reg & ", " & Tmp_Slot & ", align " & Img (Natural'Max (1, FTI.Align)));
                                             end;
                                          end;
                                       else
                                          Emit_Inst (Ctx.E, "memset " & Tmp_Slot & ", 0, " & Img (Natural'Max (1, FTI.Byte_Size)) & ", align 1");
                                       end if;
                                       Conv_Outs.Append ((
                                         Tmp_Ptr   => To_Unbounded_String (Tmp_Slot),
                                         Dst_Ptr   => LV.Reg,
                                         Src_Pir   => FTI.Pir_Type,
                                         Dst_Pir   => ITI.Pir_Type,
                                         Src_Align => Natural'Max (1, FTI.Align),
                                         Dst_Align => Natural'Max (1, ITI.Align),
                                         Conv_Type => CONST_VOID,
                                         Src_Type  => FT,
                                         Dst_Type  => LV.Ada_Type));
                                       Add_Arg (Tmp_Slot);
                                    end;
                                 end if;
                              else
                                 declare
                                    AV : constant Expr_Val := Emit_Expr (Ctx, Act, FT);
                                    Reg_S : constant String :=
                                      Emit_Scalar_Coerce (Ctx, To_String (AV.Reg), To_String (AV.Pir_Type), To_String (FTI.Pir_Type));
                                 begin
                                    Emit_Range_Check (Ctx, Reg_S, To_String (FTI.Pir_Type), FT);
                                    Emit_Discriminant_Check (Ctx, Reg_S, FT, Is_Access => True);
                                    Add_Arg (Reg_S);
                                 end;
                              end if;
                           end;
                        end loop;
                     end;
                  end loop;
               end;
            end if;

            declare
               RV : Unbounded_String;
            begin
               if not SI.Is_Function or else SI.Ret_Composite then
                  Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args_Buf), "", RV);
                  for PC of Post_Checks loop
                     declare
                        AP : constant String := Next_Reg (Ctx.E, "acout");
                     begin
                        Emit_Inst (Ctx.E, AP & " = load.ptr " & To_String (PC.Act_Ptr) & ", align 8");
                        Emit_Discriminant_Check (Ctx, AP, PC.Act_Type, Is_Access => True);
                     end;
                  end loop;
                  for CO of Conv_Outs loop
                     declare
                        Lv : constant String := Next_Reg (Ctx.E, "vco");
                        Cv : Unbounded_String;
                     begin
                        Emit_Inst (Ctx.E, Lv & " = load." & To_String (CO.Src_Pir) & " " & To_String (CO.Tmp_Ptr) & ", align " & Img (CO.Src_Align));
                        if not Is_Void_Tree (CO.Conv_Type) then
                           declare
                              CTI : constant Type_Info := Get_Type_Info (Ctx, CO.Conv_Type);
                              T1  : constant String := Emit_Typed_Scalar_Convert (Ctx, Lv, CO.Src_Type, To_String (CO.Src_Pir), CO.Conv_Type, To_String (CTI.Pir_Type));
                           begin
                              Emit_Range_Check (Ctx, T1, To_String (CTI.Pir_Type), CO.Conv_Type);
                              Cv := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, T1, CO.Conv_Type, To_String (CTI.Pir_Type), CO.Dst_Type, To_String (CO.Dst_Pir)));
                           end;
                        else
                           Cv := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, Lv, CO.Src_Type, To_String (CO.Src_Pir), CO.Dst_Type, To_String (CO.Dst_Pir)));
                        end if;
                        Emit_Range_Check (Ctx, To_String (Cv), To_String (CO.Dst_Pir), CO.Dst_Type);
                        Emit_Discriminant_Check (Ctx, To_String (Cv), CO.Dst_Type, Is_Access => True);
                        Emit_Inst (Ctx.E, "store." & To_String (CO.Dst_Pir) & " " & To_String (Cv) & ", " & To_String (CO.Dst_Ptr) & ", align " & Img (CO.Dst_Align));
                     end;
                  end loop;
                  if SI.Ret_Uncon_Arr then
                     declare
                        RTI     : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                        ETI     : constant Type_Info := Get_Type_Info (Ctx, RTI.Elem_Type);
                        ND      : constant Natural := (if RTI.NDims > 0 then RTI.NDims else 1);
                        Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, ND);
                        P_Fld   : constant String := Next_Reg (Ctx.E, "rdpf");
                        D_Ptr   : constant String := Next_Reg (Ctx.E, "rdp");
                     begin
                        Emit_Inst (Ctx.E, P_Fld & " = field_ptr " & Desc_Ty & ", " & To_String (Ret_Buf) & ", 0");
                        Emit_Inst (Ctx.E, D_Ptr & " = load.ptr " & P_Fld & ", align 8");
                        Res.Kind     := EK_Slice;
                        Res.Reg      := To_Unbounded_String (D_Ptr);
                        Res.Pir_Type := To_Unbounded_String ("ptr");
                        Res.Ada_Type := SI.Ret_Type;
                        Res.Align    := ETI.Align;
                        Res.NDims    := ND;
                        for D_Idx in 1 .. ND loop
                           declare
                              FF : constant String := Next_Reg (Ctx.E, "rdff");
                              FV : constant String := Next_Reg (Ctx.E, "rdf");
                              LF : constant String := Next_Reg (Ctx.E, "rdlf");
                              LV : constant String := Next_Reg (Ctx.E, "rdl");
                           begin
                              Emit_Inst (Ctx.E, FF & " = field_ptr " & Desc_Ty & ", " & To_String (Ret_Buf) & ", " & Img (2 * D_Idx - 1));
                              Emit_Inst (Ctx.E, FV & " = load.i32 " & FF & ", align 4");
                              Emit_Inst (Ctx.E, LF & " = field_ptr " & Desc_Ty & ", " & To_String (Ret_Buf) & ", " & Img (2 * D_Idx));
                              Emit_Inst (Ctx.E, LV & " = load.i32 " & LF & ", align 4");
                              Res.First_Regs (D_Idx) := To_Unbounded_String (FV);
                              Res.Last_Regs (D_Idx)  := To_Unbounded_String (LV);
                           end;
                        end loop;
                     end;
                  elsif SI.Ret_Composite then
                     declare
                        RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                     begin
                        Res.Kind     := EK_Addr;
                        Res.Reg      := Ret_Buf;
                        Res.Pir_Type := RTI.Pir_Type;
                        Res.Ada_Type := SI.Ret_Type;
                        Res.Align    := RTI.Align;
                     end;
                  end if;
               else
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  begin
                     Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args_Buf), To_String (RTI.Pir_Type), RV);
                     for PC of Post_Checks loop
                        declare
                           AP : constant String := Next_Reg (Ctx.E, "acout");
                        begin
                           Emit_Inst (Ctx.E, AP & " = load.ptr " & To_String (PC.Act_Ptr) & ", align 8");
                           Emit_Discriminant_Check (Ctx, AP, PC.Act_Type, Is_Access => True);
                        end;
                     end loop;
                     Res.Kind     := EK_Scalar;
                     Res.Reg      := RV;
                     Res.Pir_Type := RTI.Pir_Type;
                     Res.Ada_Type := SI.Ret_Type;
                     Res.Align    := RTI.Align;
                  end;
               end if;
            end;
            end;
            return Res;
         end;
      end;
   end Emit_Subp_Call;

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

      function Emit_Fixed_Bound_I64 (Expr_Node : TREE; Base_T : TREE) return String is
         SV : constant TREE := Safe_SM_Value (Expr_Node);
         Sm : constant IEEE_Float_64 := Fixed_Small (Base_T);
      begin
         if Has_Static_Val (SV) and then Sm > 0.0 then
            declare
               Rat : constant IEEE_Float_64 := Eval_Float64 (SV) / Sm;
               Sc  : Integer_64;
               R   : constant String := Next_Reg (Ctx.E, "fxb");
            begin
               if Rat >= IEEE_Float_64 (Integer_64'Last) then Sc := Integer_64'Last;
               elsif Rat <= IEEE_Float_64 (Integer_64'First) then Sc := Integer_64'First;
               else Sc := Integer_64 (IEEE_Float_64'Rounding (Rat));
               end if;
               Emit_Inst (Ctx.E, R & " = const.i64 " & Img (Long_Long_Integer (Sc)));
               return R;
            end;
         end if;
         declare
            EV : constant Expr_Val := Emit_Expr (Ctx, Expr_Node, Base_T);
            PT : constant String := To_String (EV.Pir_Type);
         begin
            if PT = "f32" or else PT = "f64" then
               declare
                  F64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), PT, "f64");
                  Dv  : constant String := Next_Reg (Ctx.E, "fxbd");
                  R   : constant String := Next_Reg (Ctx.E, "fxbi");
               begin
                  Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & F64 & ", " & Format_Float64 (Sm));
                  Emit_Inst (Ctx.E, R & " = fptosi.i64 " & Dv & " [sat]");
                  return R;
               end;
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
                  Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Format_Float64 (Eval_Float64 (SV)));
               elsif Is_Fixed_Type (ET) then
                  declare
                     FVal : constant IEEE_Float_64 := Eval_Float64 (SV) / Fixed_Small (ET);
                     Sc   : Integer_64;
                  begin
                     if FVal >= IEEE_Float_64 (Integer_64'Last) then Sc := Integer_64'Last;
                     elsif FVal <= IEEE_Float_64 (Integer_64'First) then Sc := Integer_64'First;
                     else Sc := Integer_64 (IEEE_Float_64'Rounding (FVal));
                     end if;
                     Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Long_Long_Integer (Sc)));
                  end;
               else
                  Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Long_Long_Integer (Eval_Int64 (SV))));
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
                              if not Is_Void_Tree (RTS) and then Ctx.Saved_Type_Ranges.Contains (Tree_Key (RTS)) then
                                 declare
                                    SR : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (RTS));
                                 begin
                                    if SR.Is_Global or else SR.Owner_Subp = Ctx.Cur_Subp_Key then
                                       declare
                                          SlotP : constant String := (if AName = "FIRST" then To_String (SR.Lo_Ptr) else To_String (SR.Hi_Ptr));
                                          RV0   : constant String := Load_Saved_Slot (Ctx, SlotP, To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                                       begin
                                          Res.Kind := EK_Scalar;
                                          Res.Reg  := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, RV0, To_String (SR.Pir_Type), PT_Str));
                                          Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                          return Res;
                                       end;
                                    end if;
                                 end;
                              end if;
                              if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                                 declare
                                    E_Bound : constant TREE := (if AName = "FIRST" then D (AS_EXP1, Rng) else D (AS_EXP2, Rng));
                                    BV : constant Expr_Val := Emit_Expr (Ctx, E_Bound, ET);
                                 begin
                                    Res.Kind := EK_Scalar;
                                    Res.Reg := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), PT_Str));
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
                                       Res.Reg  := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, Pick, To_String (RT_U), PT_Str));
                                       Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                                       return Res;
                                    end;
                                 end;
                              else
                                 declare
                                    R : constant String := Next_Reg (Ctx.E, "dbz");
                                 begin
                                    Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0");
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
                                       Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Format_Float64 (Eval_Float64 (Safe_SM_Value (E_B))));
                                       Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
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
                           Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0");
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
                                 LoV : constant Integer_64 := ATI.Dims (Dim).First;
                                 HiV : constant Integer_64 := ATI.Dims (Dim).Last;
                                 Val : Integer_64;
                                 R   : constant String := Next_Reg (Ctx.E, "ab");
                              begin
                                 if AName = "FIRST" then Val := LoV;
                                 elsif AName = "LAST" then Val := HiV;
                                 else Val := (if HiV >= LoV then HiV - LoV + 1 else 0);
                                 end if;
                                 Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (Long_Long_Integer (Val)));
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
                                    Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F_R), "i32", PT_Str));
                                 elsif AName = "LAST" then
                                    Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L_R), "i32", PT_Str));
                                 else
                                    declare
                                       L64 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (F_R), To_String (L_R));
                                    begin
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
                  -- LRM 3.6.2 (c36202c): 64-bit static constrained array object 'FIRST/'LAST/'LENGTH without i32 truncation
                  declare
                     ATI_O : constant Type_Info := Get_Type_Info (Ctx, AV.Ada_Type);
                  begin
                     if AV.Kind /= EK_Slice and then ATI_O.Kind = TK_Constrained_Array
                       and then Dim <= ATI_O.NDims and then ATI_O.Dims (Dim).Is_Static
                       and then Scalar_Bits (ATI_O.Dims (Dim).Idx_Type) > 32
                     then
                        declare
                           Lo64 : constant Integer_64 := ATI_O.Dims (Dim).First;
                           Hi64 : constant Integer_64 := ATI_O.Dims (Dim).Last;
                           R64  : constant String := Next_Reg (Ctx.E, "ao64");
                        begin
                           Res.Kind     := EK_Scalar;
                           Res.Ada_Type := ET;
                           Res.Pir_Type := To_Unbounded_String (PT_Str);
                           Res.Align    := TI.Align;
                           if AName = "FIRST" then
                              Emit_Inst (Ctx.E, R64 & " = const." & PT_Str & " " & Img (Long_Long_Integer (Lo64)));
                           elsif AName = "LAST" then
                              Emit_Inst (Ctx.E, R64 & " = const." & PT_Str & " " & Img (Long_Long_Integer (Hi64)));
                           elsif Hi64 < Lo64 then
                              Emit_Inst (Ctx.E, R64 & " = const." & PT_Str & " 0");
                           else
                              declare
                                 U_Len : constant Unsigned_64 := Unsigned_64'Mod (Hi64) - Unsigned_64'Mod (Lo64) + 1;
                              begin
                                 Emit_Inst (Ctx.E, R64 & " = const." & PT_Str & " " & Img (Long_Long_Integer (U_Len)));
                              end;
                           end if;
                           Res.Reg := To_Unbounded_String (R64);
                           return Res;
                        end;
                     end if;
                  end;
                  Get_Array_Dim_Bounds (Ctx, AV, Dim, F_R, L_R);
                  Res.Kind     := EK_Scalar;
                  Res.Ada_Type := ET;
                  Res.Pir_Type := To_Unbounded_String (PT_Str);
                  Res.Align    := TI.Align;
                  if AName = "FIRST" then
                     Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F_R), "i32", PT_Str));
                  elsif AName = "LAST" then
                     Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L_R), "i32", PT_Str));
                  else
                     declare
                        Df : constant String := Next_Reg (Ctx.E, "ld");
                        Ln : constant String := Next_Reg (Ctx.E, "ll");
                        Ps : constant String := Next_Reg (Ctx.E, "lp");
                        Cl : constant String := Next_Reg (Ctx.E, "lcl");
                     begin
                        Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L_R) & ", " & To_String (F_R));
                        Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                        Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                        Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                        Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, Cl, "i32", PT_Str));
                     end;
                  end if;
                  return Res;
               end;
            end;

         elsif AName = "POS" then
            declare
               V : constant Expr_Val := Emit_Expr (Ctx, Arg);
            begin
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := To_Unbounded_String (PT_Str);
               Res.Align    := TI.Align;
               Res.Reg      := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), PT_Str));
               return Res;
            end;

         elsif AName = "VAL" then
            declare
               V : constant Expr_Val := Emit_Expr (Ctx, Arg);
               Cv : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), PT_Str);
            begin
               Emit_Range_Check (Ctx, Cv, PT_Str, ET);
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := To_Unbounded_String (PT_Str);
               Res.Align    := TI.Align;
               Res.Reg      := To_Unbounded_String (Cv);
               return Res;
            end;

         elsif AName = "SUCC" or else AName = "PRED" then
            declare
               V   : constant Expr_Val := Emit_Expr (Ctx, Arg, ET);
               OutR : constant String := Next_Reg (Ctx.E, "sp");
               Op  : constant String := (if AName = "SUCC" then "add" else "sub");
            begin
               if Ctx.Enable_Checks then
                  declare
                     Ovf : constant String := Next_Reg (Ctx.E, "ov");
                  begin
                     Emit_Inst (Ctx.E, OutR & ", " & Ovf & " = " & Op & "_ovf_s." & PT_Str & " " & To_String (V.Reg) & ", 1");
                     Emit_Check_Fail_If (Ctx, Ovf);
                  end;
                  Emit_Range_Check (Ctx, OutR, PT_Str, ET);
               else
                  Emit_Inst (Ctx.E, OutR & " = " & Op & "." & PT_Str & " " & To_String (V.Reg) & ", 1");
               end if;
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (OutR);
               Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "SIZE" then
            if not Is_Type_Prefix (Pfx) then
               declare
                  Dummy_Pfx : constant Expr_Val := Emit_LValue (Ctx, Pfx);
               begin
                  pragma Unreferenced (Dummy_Pfx);
               end;
            end if;
            declare
               TS   : constant TREE := Subst_Resolve_Type (Ctx, Resolve_Prefix_Type (Pfx));
               BT   : constant TREE := Subst_Base_Type (Ctx, TS);
               PTI  : constant Type_Info := Get_Type_Info (Ctx, TS);
               Bits : Natural := PTI.Byte_Size * 8;
               R    : constant String := Next_Reg (Ctx.E, "sz");
               Is_Type_Pfx : Boolean := False;
               Explicit_Sz : Natural := 0;

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
                  elsif KIND (T) = DN_TASK_SPEC or else KIND (T) in CLASS_UNCONSTRAINED then
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
                     -- LRM 13.4 (cd1c04e): record representation clause size from max component bit position
                     if KIND (T) = DN_RECORD and then not Is_Void_Tree (D (SM_REPRESENTATION, T))
                       and then KIND (D (SM_REPRESENTATION, T)) = DN_RECORD_REP
                     then
                        declare
                           RR : constant TREE := D (SM_REPRESENTATION, T);
                           CL : constant TREE := D (AS_COMP_REP_S, RR);
                           Max_Bit : Natural := 0;
                        begin
                           if not Is_Void_Tree (CL) then
                              declare
                                 S : SEQ_TYPE := LIST (CL);
                                 CR : TREE;
                              begin
                                 while not IS_EMPTY (S) loop
                                    POP (S, CR);
                                    if KIND (CR) = DN_COMP_REP then
                                       declare
                                          Pos_E : constant TREE := D (AS_EXP, CR);
                                          Rng_E : constant TREE := D (AS_RANGE, CR);
                                          P_B   : Integer_64 := 0;
                                          Hi_B  : Integer_64 := 0;
                                       begin
                                          if not Is_Void_Tree (Pos_E) and then Has_Static_Val (Safe_SM_Value (Pos_E)) then
                                             P_B := Eval_Int64 (Safe_SM_Value (Pos_E));
                                          end if;
                                          if not Is_Void_Tree (Rng_E) and then KIND (Rng_E) = DN_RANGE
                                            and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, Rng_E)))
                                          then
                                             Hi_B := Eval_Int64 (Safe_SM_Value (D (AS_EXP2, Rng_E)));
                                          end if;
                                          if P_B >= 0 and then Hi_B >= 0 then
                                             declare
                                                End_B : constant Natural := Natural (P_B * 8 + Hi_B + 1);
                                             begin
                                                if End_B > Max_Bit then Max_Bit := End_B; end if;
                                             end;
                                          end if;
                                       end;
                                    end if;
                                 end loop;
                              end;
                           end if;
                           if Max_Bit > 0 then
                              return ((Max_Bit + 7) / 8) * 8;
                           end if;
                        end;
                     end if;
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
                  -- Object'SIZE must be >= Type'SIZE and >= storage byte size * 8;
                  -- for dynamic or unconstrained array objects, compute length * elem_bits at runtime!
                  if PTI.Kind = TK_Unconstrained_Array
                    or else (PTI.Kind = TK_Constrained_Array and then PTI.Is_Dynamic)
                  then
                     declare
                        AV   : constant Expr_Val := Emit_LValue (Ctx, Pfx);
                        ETI  : constant Type_Info := Get_Type_Info (Ctx, PTI.Elem_Type);
                        ESzB : constant Natural := Natural'Max (1, ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align) * 8;
                        ND   : constant Natural := Natural'Max (1, Natural'Min (PTI.NDims, 4));
                        Tot64 : Unbounded_String;
                        Bits64 : constant String := Next_Reg (Ctx.E, "dszb");
                        Conv_R : Unbounded_String;
                     begin
                        for D_I in 1 .. ND loop
                           declare
                              F_R, L_R : Unbounded_String;
                              DL       : Unbounded_String;
                           begin
                              Get_Array_Dim_Bounds (Ctx, AV, D_I, F_R, L_R);
                              DL := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_R), To_String (L_R)));
                              if D_I = 1 then
                                 Tot64 := DL;
                              else
                                 declare
                                    M : constant String := Next_Reg (Ctx.E, "dszm");
                                 begin
                                    Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & To_String (DL));
                                    Tot64 := To_Unbounded_String (M);
                                 end;
                              end if;
                           end;
                        end loop;
                        Emit_Inst (Ctx.E, Bits64 & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESzB));
                        Conv_R := To_Unbounded_String (Emit_Int_Convert (Ctx, Bits64, "i64", PT_Str));
                        Res.Kind := EK_Scalar; Res.Reg := Conv_R;
                        Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                        return Res;
                     end;
                  end if;
                  if Explicit_Sz > Bits then
                     Bits := Explicit_Sz;
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
               PDef : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Pfx)));
            begin
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
               end if;
            end;
            declare
               LV : constant Expr_Val := Emit_LValue (Ctx, Pfx);
               R  : constant String := Next_Reg (Ctx.E, "ad");
            begin
               if LV.Kind = EK_Scalar or else Length (LV.Reg) = 0 then
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
               if not St and then not Is_Void_Tree (TS)
                 and then not Is_Void_Tree (D (SM_RANGE, TS))
                 and then KIND (D (SM_RANGE, TS)) = DN_RANGE
               then
                  -- Dynamic discrete subtype 'WIDTH
                  declare
                     Rng  : constant TREE := D (SM_RANGE, TS);
                     LV   : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng), BT);
                     HV   : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng), BT);
                     Lo64 : constant String := Emit_Int_Convert (Ctx, To_String (LV.Reg), To_String (LV.Pir_Type), "i64");
                     Hi64 : constant String := Emit_Int_Convert (Ctx, To_String (HV.Reg), To_String (HV.Pir_Type), "i64");
                     RV   : Unbounded_String;
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
                             PA & ", " & Img (Cnt) & ", " & Lo64 & ", " & Hi64, "i32", RV, Nounwind => True);
                        end;
                     else
                        Emit_Call_Or_Invoke (Ctx, "@__ada_int_width", Lo64 & ", " & Hi64, "i32", RV, Nounwind => True);
                     end if;
                     Res.Kind := EK_Scalar;
                     Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", PT_Str));
                     Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
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
                  begin
                     if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
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
                     begin
                        if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
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
                                    Iv : constant String := Next_Reg (Ctx.E, "fli");
                                 begin
                                    Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (Ticks));
                                    Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (BSm / ET_Sm));
                                    Emit_Inst (Ctx.E, Iv & " = fptosi." & PT_Str & " " & Sc & " [sat]");
                                    Res.Kind := EK_Scalar;
                                    Res.Reg  := To_Unbounded_String (Iv);
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
                           Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Format_Float64 (FV));
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
                           Emit_Inst (Ctx.E, R & " = const." & PT_Str & " 0");
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
                     Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Format_Float64 (FV));
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
               Val : constant Integer := (if AName = "MACHINE_ROUNDS" then 1 else 0);
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
               BT     : constant TREE := Resolve_Type_Spec (Base_Type_Spec (TS));

               function Has_Default_Discs (Rec_T : TREE) return Boolean is
                  DS : TREE;
               begin
                  if Is_Void_Tree (Rec_T) then return False; end if;
                  if KIND (Rec_T) = DN_RECORD or else KIND (Rec_T) = DN_PRIVATE or else KIND (Rec_T) = DN_L_PRIVATE then
                     DS := D (SM_DISCRIMINANT_S, Rec_T);
                     if not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS)) then
                        declare
                           DL : SEQ_TYPE := LIST (DS);
                           DD : TREE;
                        begin
                           POP (DL, DD);
                           if KIND (DD) = DN_DSCRMT_DECL then
                              if not Is_Void_Tree (D (AS_EXP, DD)) then
                                 return True;
                              end if;
                              declare
                                 NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                 DID : TREE;
                              begin
                                 if not IS_EMPTY (NS) then
                                    POP (NS, DID);
                                    return not Is_Void_Tree (D (SM_INIT_EXP, DID));
                                 end if;
                              end;
                           end if;
                        end;
                     end if;
                  end if;
                  return False;
               end Has_Default_Discs;
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
               elsif KIND (Pfx) = DN_USED_OBJECT_ID
                 and then not Is_Void_Tree (PD)
                 and then Ctx.Vars.Contains (Tree_Key (PD))
               then
                  declare
                     VI : constant Var_Info := Ctx.Vars.Element (Tree_Key (PD));
                  begin
                     if VI.Owner_Subp = Ctx.Cur_Subp_Key and then Length (VI.Constrained_Reg) > 0 then
                        declare
                           CR : constant String := To_String (VI.Constrained_Reg);
                           R  : constant String := Emit_Int_Convert (Ctx, CR, "i8", PT_Str);
                        begin
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     end if;
                     if KIND (PD) = DN_CONSTANT_ID or else KIND (PD) = DN_IN_ID then
                        Is_Con := True;
                     else
                        declare
                           VTS : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, VI.Ada_Type));
                        begin
                           if not Is_Void_Tree (VTS) and then KIND (VTS) = DN_CONSTRAINED_RECORD then
                              Is_Con := True;
                           elsif not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_RECORD then
                              Is_Con := True;
                           elsif Has_Default_Discs (BT) then
                              Is_Con := False;
                           else
                              Is_Con := True;
                           end if;
                        end;
                     end if;
                  end;
               elsif KIND (Pfx) = DN_ALL then
                  Is_Con := True;
               elsif not Is_Void_Tree (PD) and then (KIND (PD) = DN_CONSTANT_ID or else KIND (PD) = DN_IN_ID) then
                  Is_Con := True;
               elsif not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_RECORD then
                  Is_Con := True;
               elsif Has_Default_Discs (BT) then
                  Is_Con := False;
               else
                  Is_Con := True;
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
                                                CN : constant TREE := Canonical_Def (Get_Name_Defn (D (AS_NAME, CR)));
                                             begin
                                                if CN = Def then
                                                   declare
                                                      Pos_E : constant TREE := D (AS_EXP, CR);
                                                      Rng   : constant TREE := D (AS_RANGE, CR);
                                                      Pos_V : constant Integer_64 := (if Has_Static_Val (Safe_SM_Value (Pos_E)) then Eval_Int64 (Safe_SM_Value (Pos_E)) else 0);
                                                      FB_V  : constant Integer_64 := (if not Is_Void_Tree (Rng) and then Has_Static_Val (Safe_SM_Value (D (AS_EXP1, Rng))) then Eval_Int64 (Safe_SM_Value (D (AS_EXP1, Rng))) else 0);
                                                      LB_V  : constant Integer_64 := (if not Is_Void_Tree (Rng) and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, Rng))) then Eval_Int64 (Safe_SM_Value (D (AS_EXP2, Rng))) else 7);
                                                   begin
                                                      if AName = "POSITION" then Val := Pos_V;
                                                      elsif AName = "FIRST_BIT" then Val := FB_V;
                                                      else Val := LB_V;
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
                                    Val := 0;
                                 else
                                    Val := Integer_64 (Natural'Max (1, FE.Field_Size) * 8 - 1);
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
                        if EI.Is_Family and then not Is_Void_Tree (Fam_E) then
                           declare
                              IV  : constant Expr_Val := Emit_Expr (Ctx, Fam_E);
                              I32 : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i32");
                              D1  : constant String := Next_Reg (Ctx.E, "fcd");
                              A1  : constant String := Next_Reg (Ctx.E, "fca");
                           begin
                              if EI.Fam_Is_Static then
                                 if Ctx.Enable_Checks then
                                    declare
                                       CLo : constant String := Next_Reg (Ctx.E, "fclo");
                                       CHi : constant String := Next_Reg (Ctx.E, "fchi");
                                       Bad : constant String := Next_Reg (Ctx.E, "fcob");
                                    begin
                                       Emit_Inst (Ctx.E, CLo & " = cmp_lt_s.i32 " & I32 & ", " & Img (Long_Long_Integer (EI.Fam_Lo)));
                                       Emit_Inst (Ctx.E, CHi & " = cmp_gt_s.i32 " & I32 & ", " & Img (Long_Long_Integer (EI.Fam_Hi)));
                                       Emit_Inst (Ctx.E, Bad & " = or.i8 " & CLo & ", " & CHi);
                                       Emit_Check_Fail_If (Ctx, Bad);
                                    end;
                                 end if;
                                 Emit_Inst (Ctx.E, D1 & " = sub.i32 " & I32 & ", " & Img (Long_Long_Integer (EI.Fam_Lo)));
                              else
                                 declare
                                    RT2, LoR, HiR : Unbounded_String;
                                    Lo32, Hi32 : Unbounded_String;
                                 begin
                                    Emit_Discrete_Range (Ctx, EI.Fam_DR, RT2, LoR, HiR);
                                    Lo32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LoR), To_String (RT2), "i32"));
                                    Hi32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (HiR), To_String (RT2), "i32"));
                                    if Ctx.Enable_Checks then
                                       declare
                                          CLo : constant String := Next_Reg (Ctx.E, "fclo");
                                          CHi : constant String := Next_Reg (Ctx.E, "fchi");
                                          Bad : constant String := Next_Reg (Ctx.E, "fcob");
                                       begin
                                          Emit_Inst (Ctx.E, CLo & " = cmp_lt_s.i32 " & I32 & ", " & To_String (Lo32));
                                          Emit_Inst (Ctx.E, CHi & " = cmp_gt_s.i32 " & I32 & ", " & To_String (Hi32));
                                          Emit_Inst (Ctx.E, Bad & " = or.i8 " & CLo & ", " & CHi);
                                          Emit_Check_Fail_If (Ctx, Bad);
                                       end;
                                    end if;
                                    Emit_Inst (Ctx.E, D1 & " = sub.i32 " & I32 & ", " & To_String (Lo32));
                                 end;
                              end if;
                              Emit_Inst (Ctx.E, A1 & " = add.i32 " & D1 & ", " & Img (EI.Base_Idx));
                              E_Idx := To_Unbounded_String (A1);
                           end;
                        else
                           declare
                              C : constant String := Next_Reg (Ctx.E, "eci");
                           begin
                              Emit_Inst (Ctx.E, C & " = const.i32 " & Img (EI.Base_Idx));
                              E_Idx := To_Unbounded_String (C);
                           end;
                        end if;
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
               TS  : constant TREE := Resolve_Type_Spec (Resolve_Prefix_Type (Pfx));
               BT  : constant TREE := Base_Type_Spec (TS);
            begin
               if AName = "AFT" then
                  declare
                     Del : constant IEEE_Float_64 := Fixed_Delta (TS);
                     N   : Integer := 1;
                     P10 : IEEE_Float_64 := 10.0;
                     R   : constant String := Next_Reg (Ctx.E, "fa");
                  begin
                     while N < 18 and then (P10 * Del) < 1.0 loop
                        N := N + 1;
                        P10 := P10 * 10.0;
                     end loop;
                     Emit_Inst (Ctx.E, R & " = const." & PT_Str & " " & Img (N));
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  end;
               else
                  declare
                     Rng  : constant TREE := D (SM_RANGE, TS);
                     BSm  : constant IEEE_Float_64 := Fixed_Small (BT);
                     BSR  : constant String := Next_Reg (Ctx.E, "ffbs");
                     RV   : Unbounded_String;
                  begin
                     if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                        declare
                           Lo64 : constant String := Emit_Fixed_Bound_I64 (D (AS_EXP1, Rng), BT);
                           Hi64 : constant String := Emit_Fixed_Bound_I64 (D (AS_EXP2, Rng), BT);
                        begin
                           Emit_Inst (Ctx.E, BSR & " = const.f64 " & Format_Float64 (BSm));
                           Emit_Call_Or_Invoke (Ctx, "@__ada_fixed_fore",
                             Lo64 & ", " & Hi64 & ", " & BSR, "i32", RV, Nounwind => True);
                           Res.Kind := EK_Scalar;
                           Res.Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", PT_Str));
                           Res.Pir_Type := To_Unbounded_String (PT_Str); Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
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
                  end;
               end if;
            end;

         elsif AName = "STORAGE_SIZE" then
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

   function Emit_Expr (Ctx : in out Context; Node : TREE; Target_Type : TREE := (0, 0, 0)) return Expr_Val is
      Res : Expr_Val;
      K   : constant NODE_NAME := KIND (Node);
   begin
      -- Return pre-evaluated expression value (e.g. non-discriminant expr in record comp constraint: c37213a..l)
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
                        FV  : constant IEEE_Float_64 := Eval_Float64 (SV);
                        Sm  : constant IEEE_Float_64 := Fixed_Small (ET);
                        Rat : constant IEEE_Float_64 := FV / Sm;
                        Bits : constant Natural := Scalar_Bits (Subst_Base_Type (Ctx, ET));
                        Max_T : constant IEEE_Float_64 :=
                          (if Bits >= 64 then IEEE_Float_64 (Integer_64'Last)
                           else IEEE_Float_64 (Shift_Left (Unsigned_64 (1), Bits - 1) - 1));
                        Sc  : Integer_64;
                     begin
                        if Ctx.Enable_Checks and then (Rat > Max_T or else Rat < -Max_T) then
                           Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                           declare
                              Unr : constant String := Next_Label (Ctx.E, "fxo_u");
                           begin
                              Emit_Label (Ctx.E, Unr);
                           end;
                        end if;
                        if Rat >= IEEE_Float_64 (Integer_64'Last) then Sc := Integer_64'Last;
                        elsif Rat <= IEEE_Float_64 (Integer_64'First) then Sc := Integer_64'First;
                        else Sc := Integer_64 (IEEE_Float_64'Rounding (Rat));
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
                              end if;
                           end;
                        end if;
                        Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Val)));
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
                  Mapped  : String (1 .. Len) := Text;
                  Out_Of_Range : Boolean := False;
               begin
                  -- Check user-defined character enum mapping & component subtype bounds (c42006a)
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
                                       Mapped (I) := Character'Val (Natural (Pos));
                                    else
                                       Out_Of_Range := True;
                                    end if;
                                 end;
                              end if;
                              if C_St and then (Pos < C_Lo or else Pos > C_Hi) then
                                 Out_Of_Range := True;
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

                  Add_Data_Decl (Ctx.E, Sym,
                    Sym & " = data align 1 { """ & Escape_Pir_String (Mapped) & "\0"" }");
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
                           Lo64 : constant String := Next_Reg (Ctx.E, "slf6");
                           Hi64 : constant String := Next_Reg (Ctx.E, "slh6");
                           En64 : constant String := Next_Reg (Ctx.E, "sle6");
                           L32  : constant String := Next_Reg (Ctx.E, "sll");
                        begin
                           Emit_Inst (Ctx.E, Lo64 & " = sext.i64 " & To_String (Idx_Lo));
                           Emit_Inst (Ctx.E, Hi64 & " = sext.i64 " & To_String (Idx_Hi));
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
                                          Emit_Inst (Ctx.E, Und & " = cmp_le_s.i64 " & Lo64 & ", " & Img (Long_Long_Integer (BT_Lo)));
                                          Emit_Check_Fail_If (Ctx, Und);
                                       end;
                                    end if;
                                 end;
                              end if;
                              Emit_Inst (Ctx.E, En64 & " = sub.i64 " & Lo64 & ", 1");
                           else
                              Emit_Inst (Ctx.E, En64 & " = add.i64 " & Lo64 & ", " & Img (Long_Long_Integer (Len - 1)));
                              if Ctx.Enable_Checks then
                                 declare
                                    Oob : constant String := Next_Reg (Ctx.E, "sloob");
                                 begin
                                    Emit_Inst (Ctx.E, Oob & " = cmp_gt_s.i64 " & En64 & ", " & Hi64);
                                    Emit_Check_Fail_If (Ctx, Oob);
                                 end;
                              end if;
                           end if;
                           Emit_Inst (Ctx.E, L32 & " = trunc.i32 " & En64);
                           L_R := To_Unbounded_String (L32);
                        end;
                     else
                        declare
                           FR0 : constant String := Next_Reg (Ctx.E, "sf");
                           LR0 : constant String := Next_Reg (Ctx.E, "sl");
                        begin
                           Emit_Inst (Ctx.E, FR0 & " = const.i32 1");
                           Emit_Inst (Ctx.E, LR0 & " = const.i32 " & Img (Len));
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
                           TI : constant Type_Info := Get_Type_Info (Ctx, ET);
                           R  : constant String := Next_Reg (Ctx.E, "nc");
                        begin
                           if Is_Float_Type (ET) then
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (Eval_Float64 (SV)));
                           elsif Is_Fixed_Type (ET) then
                              declare
                                 Rat : constant IEEE_Float_64 := Eval_Float64 (SV) / Fixed_Small (ET);
                                 Bits : constant Natural := Scalar_Bits (Subst_Base_Type (Ctx, ET));
                                 Max_T : constant IEEE_Float_64 :=
                                   (if Bits >= 64 then IEEE_Float_64 (Integer_64'Last)
                                    else IEEE_Float_64 (Shift_Left (Unsigned_64 (1), Bits - 1) - 1));
                                 Sc  : Integer_64;
                              begin
                                 if Ctx.Enable_Checks and then (Rat > Max_T or else Rat < -Max_T) then
                                    Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                                    declare
                                       Unr : constant String := Next_Label (Ctx.E, "fxu_u");
                                    begin
                                       Emit_Label (Ctx.E, Unr);
                                    end;
                                 end if;
                                 if Rat >= IEEE_Float_64 (Integer_64'Last) then Sc := Integer_64'Last;
                                 elsif Rat <= IEEE_Float_64 (Integer_64'First) then Sc := Integer_64'First;
                                 else Sc := Integer_64 (IEEE_Float_64'Rounding (Rat));
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
                                       end if;
                                    end;
                                 end if;
                                 Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Val)));
                              end;
                           end if;
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
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
                           declare
                              R : constant String := Next_Reg (Ctx.E, "ci");
                           begin
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (V.Const_I64)));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                              return Res;
                           end;
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
                                 P0 : constant String := Next_Reg (Ctx.E, "dp0");
                                 D0 : constant String := Next_Reg (Ctx.E, "dd0");
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
                                 Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & To_String (DP) & ", 0");
                                 Emit_Inst (Ctx.E, D0 & " = load.ptr " & P0 & ", align 8");
                                 Res.Reg := To_Unbounded_String (D0);
                                 for D_Idx in 1 .. Natural'Min (V.NDims, 4) loop
                                    declare
                                       FP : constant String := Next_Reg (Ctx.E, "dfp");
                                       FV : constant String := Next_Reg (Ctx.E, "dfv");
                                       LP : constant String := Next_Reg (Ctx.E, "dlp");
                                       LV : constant String := Next_Reg (Ctx.E, "dlv");
                                    begin
                                       Emit_Inst (Ctx.E, FP & " = field_ptr @__ada_arr_desc, " & To_String (DP) & ", " & Img (2 * D_Idx - 1));
                                       Emit_Inst (Ctx.E, FV & " = load.i32 " & FP & ", align 4");
                                       Emit_Inst (Ctx.E, LP & " = field_ptr @__ada_arr_desc, " & To_String (DP) & ", " & Img (2 * D_Idx));
                                       Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
                                       Res.First_Regs (D_Idx) := To_Unbounded_String (FV);
                                       Res.Last_Regs (D_Idx)  := To_Unbounded_String (LV);
                                    end;
                                 end loop;
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
                              Is_Shared : constant Boolean := KIND (Def) = DN_VARIABLE_ID and then DB (SM_IS_SHARED, Def);
                           begin
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
                        Is_Shared : constant Boolean := KIND (Def) = DN_VARIABLE_ID and then DB (SM_IS_SHARED, Def);
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
                                 Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8");
                                 Rec_Ptr := To_Unbounded_String (LP);
                              end;
                           end if;
                           Emit_Null_Check (Ctx, To_String (Rec_Ptr));
                        end if;
                        if CTI.Kind = TK_Scalar then
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
                        Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (TI.Align));
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
            return Emit_Expr (Ctx, D (AS_EXP, Node), Target_Type);

         when DN_QUALIFIED =>
            declare
               ET  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Node)));
               Exp_N : constant TREE := D (AS_EXP, Node);
               E   : constant Expr_Val := Emit_Expr (Ctx, Exp_N, ET);
               ETI : constant Type_Info := Get_Type_Info (Ctx, ET);
            begin
               if E.Kind = EK_Scalar then
                  Emit_Range_Check (Ctx, To_String (E.Reg), To_String (E.Pir_Type), ET);
                  Emit_Discriminant_Check (Ctx, To_String (E.Reg), ET, Is_Access => True);
               elsif ETI.Kind = TK_Record then
                  Emit_Discriminant_Check (Ctx, To_String (E.Reg), ET, Is_Access => False);
               elsif ETI.Kind = TK_Constrained_Array and then Ctx.Enable_Checks then
                  -- LRM 4.7(3) (c47007a, c48009d/e/f): qualified array checks EXACT bounds (no sliding!),
                  -- except string literals take their lower bound from ET'FIRST (so check length).
                  declare
                     Tgt : Expr_Val;
                     ND_A : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, 4));
                  begin
                     Tgt.Kind := EK_Slice; Tgt.Ada_Type := ET; Tgt.NDims := ND_A;
                     for D_I in 1 .. ND_A loop
                        Emit_Index_Bounds (Ctx, ETI.Dims (D_I).Idx_Type, Tgt.First_Regs (D_I), Tgt.Last_Regs (D_I));
                     end loop;
                     if not Is_Void_Tree (Exp_N) and then KIND (Exp_N) = DN_STRING_LITERAL then
                        Emit_Array_Length_Check (Ctx, Tgt, E, ND_A);
                     else
                        for D_I in 1 .. ND_A loop
                           declare
                              EF, EL : Unbounded_String;
                              C1  : constant String := Next_Reg (Ctx.E, "qb1");
                              C2  : constant String := Next_Reg (Ctx.E, "qb2");
                              Bad : constant String := Next_Reg (Ctx.E, "qbb");
                           begin
                              Get_Array_Dim_Bounds (Ctx, E, D_I, EF, EL);
                              Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (EF) & ", " & To_String (Tgt.First_Regs (D_I)));
                              Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (EL) & ", " & To_String (Tgt.Last_Regs (D_I)));
                              Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                              Emit_Check_Fail_If (Ctx, Bad);
                           end;
                        end loop;
                     end if;
                  end;
               end if;
               return E;
            end;

         when DN_CONVERSION =>
            declare
               ET  : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               ETI : constant Type_Info := Get_Type_Info (Ctx, ET);
               Src : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Node));
               ST  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Src.Ada_Type));
            begin
               if ETI.Kind /= TK_Scalar then
                  if ETI.Kind = TK_Constrained_Array or else ETI.Kind = TK_Unconstrained_Array then
                     declare
                        STI_C : constant Type_Info := Get_Type_Info (Ctx, Src.Ada_Type);
                        ND : constant Natural := Natural'Max (1, Natural'Min (ETI.NDims, 4));
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
                              for D_I in 1 .. ND loop
                                 declare
                                    NE    : constant String := Next_Reg (Ctx.E, "acne");
                                    L_Chk : constant String := Next_Label (Ctx.E, "acc");
                                    L_Ok  : constant String := Next_Label (Ctx.E, "aco");
                                    IT    : constant TREE := ETI.Dims (D_I).Idx_Type;
                                    ITI   : constant Type_Info := Get_Type_Info (Ctx, IT);
                                 begin
                                    Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (SF (D_I)) & ", " & To_String (SL (D_I)));
                                    Emit_Term (Ctx.E, "br " & NE & ", " & L_Chk & ", " & L_Ok);
                                    Emit_Label (Ctx.E, L_Chk);
                                    Emit_Range_Check (Ctx, To_String (SF (D_I)), "i32", IT);
                                    Emit_Range_Check (Ctx, To_String (SL (D_I)), "i32", IT);
                                    Emit_Term (Ctx.E, "jmp " & L_Ok);
                                    Emit_Label (Ctx.E, L_Ok);
                                 end;
                              end loop;
                           end if;
                           Res.First_Regs := SF;
                           Res.Last_Regs  := SL;
                        end if;
                        return Res;
                     end;
                  elsif ETI.Kind = TK_Record then
                     Emit_Discriminant_Check (Ctx, To_String (Src.Reg), ET, Is_Access => False);
                     -- LRM 13.6: derived record change of representation when source/target have different record reps
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
                              STI_R : constant Type_Info := Get_Type_Info (Ctx, Src_BR);
                              DTI_R : constant Type_Info := Get_Type_Info (Ctx, Dst_BR);
                              Buf   : constant String := Next_Reg (Ctx.E, "crb");

                              function Find_Comp_By_Name (BR : TREE; Want : String) return TREE is
                                 function Search_DS (DS : TREE) return TREE is
                                    DL : SEQ_TYPE;
                                    DD : TREE;
                                 begin
                                    if Is_Void_Tree (DS) then return CONST_VOID; end if;
                                    DL := LIST (DS);
                                    while not IS_EMPTY (DL) loop
                                       POP (DL, DD);
                                       if KIND (DD) in DN_DSCRMT_DECL | DN_VARIABLE_DECL then
                                          declare
                                             NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                             ID : TREE;
                                          begin
                                             while not IS_EMPTY (NS) loop
                                                POP (NS, ID);
                                                if Sym_Name (ID) = Want then
                                                   return Canonical_Def (ID);
                                                end if;
                                             end loop;
                                          end;
                                       end if;
                                    end loop;
                                    return CONST_VOID;
                                 end Search_DS;

                                 function Search_CL (CL : TREE) return TREE is
                                    R  : TREE;
                                    VP : TREE;
                                 begin
                                    if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return CONST_VOID; end if;
                                    R := Search_DS (D (AS_DECL_S, CL));
                                    if not Is_Void_Tree (R) then return R; end if;
                                    VP := D (AS_VARIANT_PART, CL);
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
                                                R := Search_CL (D (AS_COMP_LIST, VN));
                                                if not Is_Void_Tree (R) then return R; end if;
                                             end if;
                                          end loop;
                                       end;
                                    end if;
                                    return CONST_VOID;
                                 end Search_CL;
                                 R : TREE;
                              begin
                                 R := Search_DS (D (SM_DISCRIMINANT_S, BR));
                                 if not Is_Void_Tree (R) then return R; end if;
                                 return Search_CL (D (SM_COMP_LIST, BR));
                              end Find_Comp_By_Name;

                              procedure Copy_Comp (Dst_CID : TREE) is
                                 Can_D   : constant TREE := Canonical_Def (Dst_CID);
                                 Src_CID : constant TREE := Find_Comp_By_Name (Src_BR, Sym_Name (Can_D));
                                 DK      : constant Unsigned_32 := Tree_Key (Can_D);
                                 SK      : Unsigned_32;
                              begin
                                 if Is_Void_Tree (Src_CID) or else not Ctx.Fields.Contains (DK) then return; end if;
                                 SK := Tree_Key (Src_CID);
                                 if not Ctx.Fields.Contains (SK) then return; end if;
                                 declare
                                    SFE : constant Field_Entry := Ctx.Fields.Element (SK);
                                    DFE : constant Field_Entry := Ctx.Fields.Element (DK);
                                    CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Can_D));
                                    CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                 begin
                                    if CTI.Kind = TK_Scalar then
                                       declare
                                          VR : Unbounded_String;
                                       begin
                                          if Is_Bitfield (SFE) then
                                             VR := To_Unbounded_String (Emit_Bitfield_Load (Ctx, To_String (Src.Reg), SFE, CT));
                                          else
                                             declare
                                                SP : constant String := Next_Reg (Ctx.E, "crsp");
                                                SV : constant String := Next_Reg (Ctx.E, "crsv");
                                             begin
                                                if SFE.In_Union or else SFE.Has_Explicit_Offset then
                                                   declare
                                                      SO : constant String := Next_Reg (Ctx.E, "crso");
                                                   begin
                                                      Emit_Inst (Ctx.E, SO & " = const.i64 " & Img (SFE.Byte_Offset));
                                                      Emit_Inst (Ctx.E, SP & " = ptr_add.i64 " & To_String (Src.Reg) & ", " & SO & " [inbounds]");
                                                   end;
                                                else
                                                   Emit_Inst (Ctx.E, SP & " = field_ptr " & To_String (STI_R.Pir_Type) & ", " & To_String (Src.Reg) & ", " & Img (SFE.Field_Idx));
                                                end if;
                                                Emit_Inst (Ctx.E, SV & " = load." & To_String (CTI.Pir_Type) & " " & SP & ", align " & Img (CTI.Align));
                                                VR := To_Unbounded_String (SV);
                                             end;
                                          end if;
                                          if Is_Bitfield (DFE) then
                                             Emit_Bitfield_Store (Ctx, Buf, DFE, To_String (VR), To_String (CTI.Pir_Type), CT);
                                          else
                                             declare
                                                DP : constant String := Next_Reg (Ctx.E, "crdp");
                                             begin
                                                if DFE.In_Union or else DFE.Has_Explicit_Offset then
                                                   declare
                                                      DO_R : constant String := Next_Reg (Ctx.E, "crdo");
                                                   begin
                                                      Emit_Inst (Ctx.E, DO_R & " = const.i64 " & Img (DFE.Byte_Offset));
                                                      Emit_Inst (Ctx.E, DP & " = ptr_add.i64 " & Buf & ", " & DO_R & " [inbounds]");
                                                   end;
                                                else
                                                   Emit_Inst (Ctx.E, DP & " = field_ptr " & To_String (DTI_R.Pir_Type) & ", " & Buf & ", " & Img (DFE.Field_Idx));
                                                end if;
                                                Emit_Inst (Ctx.E, "store." & To_String (CTI.Pir_Type) & " " & To_String (VR) & ", " & DP & ", align " & Img (CTI.Align));
                                             end;
                                          end if;
                                       end;
                                    else
                                       declare
                                          SP : constant String := Next_Reg (Ctx.E, "crsp");
                                          DP : constant String := Next_Reg (Ctx.E, "crdp");
                                       begin
                                          if SFE.In_Union or else SFE.Has_Explicit_Offset then
                                             declare
                                                SO : constant String := Next_Reg (Ctx.E, "crso");
                                             begin
                                                Emit_Inst (Ctx.E, SO & " = const.i64 " & Img (SFE.Byte_Offset));
                                                Emit_Inst (Ctx.E, SP & " = ptr_add.i64 " & To_String (Src.Reg) & ", " & SO & " [inbounds]");
                                             end;
                                          else
                                             Emit_Inst (Ctx.E, SP & " = field_ptr " & To_String (STI_R.Pir_Type) & ", " & To_String (Src.Reg) & ", " & Img (SFE.Field_Idx));
                                          end if;
                                          if DFE.In_Union or else DFE.Has_Explicit_Offset then
                                             declare
                                                DO_R : constant String := Next_Reg (Ctx.E, "crdo");
                                             begin
                                                Emit_Inst (Ctx.E, DO_R & " = const.i64 " & Img (DFE.Byte_Offset));
                                                Emit_Inst (Ctx.E, DP & " = ptr_add.i64 " & Buf & ", " & DO_R & " [inbounds]");
                                             end;
                                          else
                                             Emit_Inst (Ctx.E, DP & " = field_ptr " & To_String (DTI_R.Pir_Type) & ", " & Buf & ", " & Img (DFE.Field_Idx));
                                          end if;
                                          Emit_Inst (Ctx.E, "memcpy " & DP & ", " & SP & ", " & Img (Natural'Max (1, CTI.Byte_Size)) & ", align 1");
                                       end;
                                    end if;
                                 end;
                              end Copy_Comp;

                              procedure Copy_DS (DS : TREE) is
                                 DL : SEQ_TYPE;
                                 DD : TREE;
                              begin
                                 if Is_Void_Tree (DS) then return; end if;
                                 DL := LIST (DS);
                                 while not IS_EMPTY (DL) loop
                                    POP (DL, DD);
                                    if KIND (DD) in DN_DSCRMT_DECL | DN_VARIABLE_DECL then
                                       declare
                                          NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DD));
                                          ID : TREE;
                                       begin
                                          while not IS_EMPTY (NS) loop
                                             POP (NS, ID);
                                             Copy_Comp (ID);
                                          end loop;
                                       end;
                                    end if;
                                 end loop;
                              end Copy_DS;

                              procedure Copy_CL (CL : TREE) is
                                 VP : TREE;
                              begin
                                 if Is_Void_Tree (CL) or else KIND (CL) /= DN_COMP_LIST then return; end if;
                                 Copy_DS (D (AS_DECL_S, CL));
                                 VP := D (AS_VARIANT_PART, CL);
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
                                             Copy_CL (D (AS_COMP_LIST, VN));
                                          end if;
                                       end loop;
                                    end;
                                 end if;
                              end Copy_CL;
                           begin
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, DTI_R.Byte_Size)) & " x i8], align " & Img (Natural'Max (1, DTI_R.Align)));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (Natural'Max (1, DTI_R.Byte_Size)) & ", align 1");
                              Copy_DS (D (SM_DISCRIMINANT_S, Dst_BR));
                              Copy_CL (D (SM_COMP_LIST, Dst_BR));
                              Res.Kind     := EK_Addr;
                              Res.Reg      := To_Unbounded_String (Buf);
                              Res.Pir_Type := DTI_R.Pir_Type;
                              Res.Ada_Type := ET;
                              Res.Align    := DTI_R.Align;
                              return Res;
                           end;
                        end if;
                     end;
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

               if Is_Float_Type (ET) and then Is_Float_Type (ST) then
                  if To_String (Src.Pir_Type) = To_String (ETI.Pir_Type) then
                     Res.Reg := Src.Reg;
                     Emit_Range_Check (Ctx, To_String (Res.Reg), To_String (ETI.Pir_Type), ET);
                  else
                     declare
                        R : constant String := Next_Reg (Ctx.E, "fc");
                     begin
                        if To_String (ETI.Pir_Type) = "f64" then
                           Emit_Inst (Ctx.E, R & " = fpext.f64 " & To_String (Src.Reg));
                           Emit_Range_Check (Ctx, R, "f64", ET);
                        else
                           Emit_Range_Check (Ctx, To_String (Src.Reg), "f64", ET);
                           Emit_Inst (Ctx.E, R & " = fptrunc.f32 " & To_String (Src.Reg));
                        end if;
                        Res.Reg := To_Unbounded_String (R);
                     end;
                  end if;
               elsif Is_Float_Type (ET) and then Is_Fixed_Type (ST) then
                  declare
                     Fl : constant String := Next_Reg (Ctx.E, "x2f");
                     Sc : constant String := Next_Reg (Ctx.E, "xsc");
                     Sm : constant IEEE_Float_64 := Fixed_Small (ST);
                  begin
                     Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (Src.Reg));
                     Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Sm));
                     if To_String (ETI.Pir_Type) = "f32" then
                        declare
                           Tr : constant String := Next_Reg (Ctx.E, "xtr");
                        begin
                           Emit_Range_Check (Ctx, Sc, "f64", ET);
                           Emit_Inst (Ctx.E, Tr & " = fptrunc.f32 " & Sc);
                           Res.Reg := To_Unbounded_String (Tr);
                        end;
                     else
                        Emit_Range_Check (Ctx, Sc, "f64", ET);
                        Res.Reg := To_Unbounded_String (Sc);
                     end if;
                  end;
               elsif Is_Fixed_Type (ET) and then (Is_Float_Type (ST) or else To_String (Src.Pir_Type) = "f64" or else To_String (Src.Pir_Type) = "f32") then
                  declare
                     F64 : Unbounded_String := Src.Reg;
                     Div : constant String := Next_Reg (Ctx.E, "f2x");
                     V64 : Unbounded_String;
                     R   : Unbounded_String;
                     Sm  : constant IEEE_Float_64 := Fixed_Small (ET);
                     DPT : constant String := To_String (ETI.Pir_Type);
                     Chk : constant String := (if Ctx.Enable_Checks then "1" else "0");
                     MinB : constant Long_Long_Integer := (if DPT = "i32" then -2147483648 else Long_Long_Integer'First);
                     MaxB : constant Long_Long_Integer := (if DPT = "i32" then 2147483647 else Long_Long_Integer'Last);
                  begin
                     if To_String (Src.Pir_Type) = "f32" then
                        declare
                           Ex : constant String := Next_Reg (Ctx.E, "fex");
                        begin
                           Emit_Inst (Ctx.E, Ex & " = fpext.f64 " & To_String (Src.Reg));
                           F64 := To_Unbounded_String (Ex);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, Div & " = fdiv.f64 " & To_String (F64) & ", " & Format_Float64 (Sm));
                     Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_trunc",
                       Div & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                     R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", DPT));
                     Emit_Range_Check (Ctx, To_String (R), DPT, ET);
                     Res.Reg := R;
                  end;
               elsif Is_Fixed_Type (ET) and then Is_Fixed_Type (ST) then
                  Res.Reg := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, To_String (Src.Reg), ST, To_String (Src.Pir_Type), ET, To_String (ETI.Pir_Type)));
                  Emit_Range_Check (Ctx, To_String (Res.Reg), To_String (ETI.Pir_Type), ET);
               elsif Is_Fixed_Type (ET) and then Is_Discrete_Type (ST) then
                  declare
                     Fl  : constant String := Next_Reg (Ctx.E, "i2xf");
                     Dv  : constant String := Next_Reg (Ctx.E, "i2xd");
                     V64 : Unbounded_String;
                     R   : Unbounded_String;
                     DPT : constant String := To_String (ETI.Pir_Type);
                     Chk : constant String := (if Ctx.Enable_Checks then "1" else "0");
                     MinB : constant Long_Long_Integer := (if DPT = "i32" then -2147483648 else Long_Long_Integer'First);
                     MaxB : constant Long_Long_Integer := (if DPT = "i32" then 2147483647 else Long_Long_Integer'Last);
                  begin
                     Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (Src.Reg));
                     Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (ET)));
                     Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_trunc",
                       Dv & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                     R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", DPT));
                     Emit_Range_Check (Ctx, To_String (R), DPT, ET);
                     Res.Reg := R;
                  end;
               elsif Is_Discrete_Type (ET) and then Is_Fixed_Type (ST) then
                  declare
                     Fl   : constant String := Next_Reg (Ctx.E, "x2if");
                     Sc   : constant String := Next_Reg (Ctx.E, "x2is");
                     V64  : Unbounded_String;
                     R    : Unbounded_String;
                     DPT  : constant String := To_String (ETI.Pir_Type);
                     Chk  : constant String := (if Ctx.Enable_Checks then "1" else "0");
                     MinB : constant Long_Long_Integer := Min_Signed_For_Pir (DPT);
                     MaxB : constant Long_Long_Integer :=
                       (if DPT = "i8" then 127 elsif DPT = "i16" then 32767
                        elsif DPT = "i32" then 2147483647 else Long_Long_Integer'Last);
                  begin
                     Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (Src.Reg));
                     Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (ST)));
                     Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                       Sc & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                     R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", DPT));
                     Emit_Range_Check (Ctx, To_String (R), DPT, ET);
                     Res.Reg := R;
                  end;
               elsif Is_Float_Type (ET) and then Is_Discrete_Type (ST) then
                  declare
                     R : constant String := Next_Reg (Ctx.E, "i2f");
                  begin
                     Emit_Inst (Ctx.E, R & " = sitofp." & To_String (ETI.Pir_Type) & " " & To_String (Src.Reg));
                     Emit_Range_Check (Ctx, R, To_String (ETI.Pir_Type), ET);
                     Res.Reg := To_Unbounded_String (R);
                  end;
               elsif Is_Discrete_Type (ET) and then (Is_Float_Type (ST) or else To_String (Src.Pir_Type) = "f64" or else To_String (Src.Pir_Type) = "f32") then
                  -- Ada LRM 4.6(7): round halfway away from zero using exact C99 round() without double-rounding or silent saturation!
                  declare
                     F64  : Unbounded_String := Src.Reg;
                     V64  : Unbounded_String;
                     R    : Unbounded_String;
                     DPT  : constant String := To_String (ETI.Pir_Type);
                     Chk  : constant String := (if Ctx.Enable_Checks then "1" else "0");
                     MinB : constant Long_Long_Integer := Min_Signed_For_Pir (DPT);
                     MaxB : constant Long_Long_Integer :=
                       (if DPT = "i8" then 127 elsif DPT = "i16" then 32767
                        elsif DPT = "i32" then 2147483647 else Long_Long_Integer'Last);
                  begin
                     if To_String (Src.Pir_Type) = "f32" then
                        declare
                           Ex : constant String := Next_Reg (Ctx.E, "fex");
                        begin
                           Emit_Inst (Ctx.E, Ex & " = fpext.f64 " & To_String (Src.Reg));
                           F64 := To_Unbounded_String (Ex);
                        end;
                     end if;
                     Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_round",
                       To_String (F64) & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                     R := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V64), "i64", DPT));
                     Emit_Range_Check (Ctx, To_String (R), DPT, ET);
                     Res.Reg := R;
                  end;
               else
                  -- Integer / discrete conversion: range-check in wider width before narrowing! (c46014a)
                  declare
                     SBits : constant Natural := Int_Bits_Of_Pir (To_String (Src.Pir_Type));
                     DBits : constant Natural := Int_Bits_Of_Pir (To_String (ETI.Pir_Type));
                  begin
                     if Ctx.Enable_Checks and then SBits > DBits then
                        Emit_Range_Check (Ctx, To_String (Src.Reg), To_String (Src.Pir_Type), ET);
                     end if;
                     declare
                        Cv : constant String :=
                          Emit_Int_Convert (Ctx, To_String (Src.Reg), To_String (Src.Pir_Type), To_String (ETI.Pir_Type));
                     begin
                        if not (Ctx.Enable_Checks and then SBits > DBits) then
                           Emit_Range_Check (Ctx, Cv, To_String (ETI.Pir_Type), ET);
                        end if;
                        Res.Reg := To_Unbounded_String (Cv);
                     end;
                  end;
               end if;
               return Res;
            end;

         when DN_SHORT_CIRCUIT =>
            declare
               Is_And : constant Boolean := KIND (D (AS_SHORT_CIRCUIT_OP, Node)) = DN_AND_THEN;
               L      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Node));
               L_Rhs  : constant String := Next_Label (Ctx.E, "sc_rhs");
               L_End  : constant String := Next_Label (Ctx.E, "sc_end");
               OutR   : constant String := Next_Reg (Ctx.E, "sc");
            begin
               if Is_And then
                  Emit_Term (Ctx.E, "br " & To_String (L.Reg) & ", " & L_Rhs & ", " & L_End & "(0)");
               else
                  Emit_Term (Ctx.E, "br " & To_String (L.Reg) & ", " & L_End & "(1), " & L_Rhs);
               end if;
               Emit_Label (Ctx.E, L_Rhs);
               declare
                  R_V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Node));
               begin
                  Emit_Term (Ctx.E, "jmp " & L_End & "(" & To_String (R_V.Reg) & ")");
               end;
               Emit_Label (Ctx.E, L_End, OutR & ": i8");
               Res.Kind     := EK_Scalar;
               Res.Reg      := To_Unbounded_String (OutR);
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               Res.Align    := 1;
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
                  PTS : constant String := To_String (RT);
               begin
                  VC := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), PTS));
                  if PTS = "f32" or else PTS = "f64" then
                     Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & PTS & " " & To_String (VC) & ", " & To_String (RL));
                     Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & PTS & " " & To_String (VC) & ", " & To_String (RH));
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
               if St then
                  declare
                     PT : constant String := To_String (V.Pir_Type);
                     C1 : constant String := Next_Reg (Ctx.E, "tm1");
                     C2 : constant String := Next_Reg (Ctx.E, "tm2");
                  begin
                     Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Lo)));
                     Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Hi)));
                     Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
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
                  begin
                     if KIND (Rng) = DN_RANGE then
                        declare
                           E1 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng), TS);
                           E2 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng), TS);
                        begin
                           L_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), PT));
                           H_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), PT));
                        end;
                     else
                        declare
                           RT2, LoR, HiR : Unbounded_String;
                        begin
                           Emit_Discrete_Range (Ctx, Rng, RT2, LoR, HiR);
                           L_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (LoR), To_String (RT2), PT));
                           H_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (HiR), To_String (RT2), PT));
                        end;
                     end if;
                     if PT = "f32" or else PT = "f64" then
                        Emit_Inst (Ctx.E, C1 & " = fcmp_oge." & PT & " " & To_String (V.Reg) & ", " & To_String (L_R));
                        Emit_Inst (Ctx.E, C2 & " = fcmp_ole." & PT & " " & To_String (V.Reg) & ", " & To_String (H_R));
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
                     ND  : constant Natural := Natural'Max (1, Natural'Min (ATI.NDims, 4));
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
                           Emit_Inst (Ctx.E, C1 & " = cmp_eq.i32 " & To_String (VF) & ", " & To_String (TF));
                           Emit_Inst (Ctx.E, C2 & " = cmp_eq.i32 " & To_String (VL) & ", " & To_String (TL));
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
                     BT  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (TS));
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
                                          DT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DID));
                                          DTI : constant Type_Info := Get_Type_Info (Ctx, DT);
                                          FP  : constant String := Next_Reg (Ctx.E, "rmp");
                                          AV  : constant String := Next_Reg (Ctx.E, "rmv");
                                          EV  : constant Expr_Val := Emit_Expr (Ctx, CE, DT);
                                          ER  : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), To_String (DTI.Pir_Type));
                                          Eq  : constant String := Next_Reg (Ctx.E, "rmq");
                                       begin
                                          Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (V.Reg) & ", " & Img (FIdx));
                                          Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (DTI.Align));
                                          Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & ER);
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
                           ND  : constant Natural := Natural'Max (1, Natural'Min (ATI.NDims, 4));
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
                                 Emit_Inst (Ctx.E, C1 & " = cmp_eq.i32 " & To_String (Unp.First_Regs (D_I)) & ", " & To_String (TF));
                                 Emit_Inst (Ctx.E, C2 & " = cmp_eq.i32 " & To_String (Unp.Last_Regs (D_I)) & ", " & To_String (TL));
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
                           BT  : constant TREE := Resolve_Type_Spec (Base_Type_Spec (DesT));
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
                                                FP  : constant String := Next_Reg (Ctx.E, "arp");
                                                AV  : constant String := Next_Reg (Ctx.E, "arv");
                                                EV  : constant Expr_Val := Emit_Expr (Ctx, CE, DT);
                                                ER  : constant String := Emit_Scalar_Coerce (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), To_String (DTI.Pir_Type));
                                                Eq  : constant String := Next_Reg (Ctx.E, "arq");
                                             begin
                                                Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & To_String (V.Reg) & ", " & Img (FIdx));
                                                Emit_Inst (Ctx.E, AV & " = load." & To_String (DTI.Pir_Type) & " " & FP & ", align " & Img (DTI.Align));
                                                Emit_Inst (Ctx.E, Eq & " = cmp_eq." & To_String (DTI.Pir_Type) & " " & AV & ", " & ER);
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
                        ESz    : constant Natural := (if Is_DE then 40 else ((ETI.Byte_Size + EAl - 1) / EAl) * EAl);
                        ND     : constant Natural := Natural'Max (1, Natural'Min (TI.NDims, 4));
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
                        -- Check if aggregate (or any nested subaggregate) uses OTHERS
                        declare
                           function Rec_Has_Others (AN : TREE) return Boolean is
                           begin
                              if Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then
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
                                       if Rec_Has_Others (D (AS_EXP, IT)) then
                                          return True;
                                       end if;
                                    else
                                       if Rec_Has_Others (IT) then
                                          return True;
                                       end if;
                                    end if;
                                 end loop;
                              end;
                              return False;
                           end Rec_Has_Others;
                        begin
                           Has_Others := Rec_Has_Others (Node);
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
                                                      L32, H32 : Unbounded_String;
                                                      SR : Saved_Range_Info;
                                                   begin
                                                      Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                                      if Ctx.Enable_Checks and then To_String (RT) = "i64"
                                                        and then Dim_I <= Base_TI.NDims and then not Is_Void_Tree (Base_TI.Dims (Dim_I).Idx_Type)
                                                      then
                                                         declare
                                                            NE64  : constant String := Next_Reg (Ctx.E, "p164ne");
                                                            L_C64 : constant String := Next_Label (Ctx.E, "p164c");
                                                            L_O64 : constant String := Next_Label (Ctx.E, "p164o");
                                                         begin
                                                            Emit_Inst (Ctx.E, NE64 & " = cmp_le_s.i64 " & To_String (RL) & ", " & To_String (RH));
                                                            Emit_Term (Ctx.E, "br " & NE64 & ", " & L_C64 & ", " & L_O64);
                                                            Emit_Label (Ctx.E, L_C64);
                                                            Emit_Range_Check (Ctx, To_String (RL), "i64", Base_TI.Dims (Dim_I).Idx_Type);
                                                            Emit_Range_Check (Ctx, To_String (RH), "i64", Base_TI.Dims (Dim_I).Idx_Type);
                                                            Emit_Term (Ctx.E, "jmp " & L_O64);
                                                            Emit_Label (Ctx.E, L_O64);
                                                         end;
                                                      end if;
                                                      L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                                                      H32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                                                      -- Cache Pass-1 choice range so Pass 2 does NOT re-evaluate side effects (c43207b/c, c43208b)
                                                      Save_Scalar_Slot (Ctx, To_String (L32), "i32", 4, SR.Lo_Ptr, SR.Is_Global);
                                                      Save_Scalar_Slot (Ctx, To_String (H32), "i32", 4, SR.Hi_Ptr, SR.Is_Global);
                                                      SR.Pir_Type   := To_Unbounded_String ("i32");
                                                      SR.Align      := 4;
                                                      SR.Owner_Subp := Ctx.Cur_Subp_Key;
                                                      Ctx.Saved_Type_Ranges.Include (Tree_Key (Ch), SR);
                                                      Temp_Rng_Keys.Append (Tree_Key (Ch));
                                                      -- Check non-null choice range against BASE index subtype even if outer dim is null (c43206a C1)
                                                      if Ctx.Enable_Checks and then Dim_I <= Base_TI.NDims and then not Is_Void_Tree (Base_TI.Dims (Dim_I).Idx_Type) then
                                                         declare
                                                            NE    : constant String := Next_Reg (Ctx.E, "p1ne");
                                                            L_Chk : constant String := Next_Label (Ctx.E, "p1c");
                                                            L_Ok  : constant String := Next_Label (Ctx.E, "p1o");
                                                            BIT   : constant TREE := Base_TI.Dims (Dim_I).Idx_Type;
                                                            BITI  : constant Type_Info := Get_Type_Info (Ctx, BIT);
                                                         begin
                                                            Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (L32) & ", " & To_String (H32));
                                                            Emit_Term (Ctx.E, "br " & NE & ", " & L_Chk & ", " & L_Ok);
                                                            Emit_Label (Ctx.E, L_Chk);
                                                            declare
                                                               LC : constant String := Emit_Int_Convert (Ctx, To_String (L32), "i32", To_String (BITI.Pir_Type));
                                                               HC : constant String := Emit_Int_Convert (Ctx, To_String (H32), "i32", To_String (BITI.Pir_Type));
                                                            begin
                                                               Emit_Range_Check (Ctx, LC, To_String (BITI.Pir_Type), BIT);
                                                               Emit_Range_Check (Ctx, HC, To_String (BITI.Pir_Type), BIT);
                                                            end;
                                                            Emit_Term (Ctx.E, "jmp " & L_Ok);
                                                            Emit_Label (Ctx.E, L_Ok);
                                                         end;
                                                      end if;
                                                      if not Got_R then
                                                         Firsts (Dim_I) := L32;
                                                         Lasts (Dim_I)  := H32;
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "amn");
                                                            ML : constant String := Next_Reg (Ctx.E, "alo");
                                                            CH : constant String := Next_Reg (Ctx.E, "amx");
                                                            MH : constant String := Next_Reg (Ctx.E, "ahi");
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
                                                         Temp_Exp_Keys.Append (Tree_Key (ChE));
                                                      end if;
                                                      if Ctx.Enable_Checks and then Dim_I <= Base_TI.NDims and then not Is_Void_Tree (Base_TI.Dims (Dim_I).Idx_Type) then
                                                         declare
                                                            BIT  : constant TREE := Base_TI.Dims (Dim_I).Idx_Type;
                                                            BITI : constant Type_Info := Get_Type_Info (Ctx, BIT);
                                                            CC   : constant String := Emit_Int_Convert (Ctx, C32, "i32", To_String (BITI.Pir_Type));
                                                         begin
                                                            Emit_Range_Check (Ctx, CC, To_String (BITI.Pir_Type), BIT);
                                                         end;
                                                      end if;
                                                      if not Got_R then
                                                         Firsts (Dim_I) := To_Unbounded_String (C32);
                                                         Lasts (Dim_I)  := To_Unbounded_String (C32);
                                                         Got_R := True;
                                                      else
                                                         declare
                                                            CL : constant String := Next_Reg (Ctx.E, "amn");
                                                            ML : constant String := Next_Reg (Ctx.E, "alo");
                                                            CH : constant String := Next_Reg (Ctx.E, "amx");
                                                            MH : constant String := Next_Reg (Ctx.E, "ahi");
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
                                       if not Is_Void_Tree (Cur_Agg) and then KIND (Cur_Agg) = DN_STRING_LITERAL then
                                          declare
                                             STxt : constant String := Decode_Ada_String_Lit (Sym_Name (Cur_Agg));
                                          begin
                                             Count := Integer_64 (STxt'Length);
                                          end;
                                       end if;
                                       declare
                                          Idx_Lo_R, Idx_Hi_R : Unbounded_String;
                                          Lo64 : constant String := Next_Reg (Ctx.E, "al64");
                                          Hi64 : constant String := Next_Reg (Ctx.E, "ah64");
                                          En64 : constant String := Next_Reg (Ctx.E, "ae64");
                                          L32  : constant String := Next_Reg (Ctx.E, "agl");
                                       begin
                                          Emit_Index_Bounds (Ctx, TI.Dims (Dim_I).Idx_Type, Idx_Lo_R, Idx_Hi_R);
                                          Emit_Inst (Ctx.E, Lo64 & " = sext.i64 " & To_String (Idx_Lo_R));
                                          Emit_Inst (Ctx.E, Hi64 & " = sext.i64 " & To_String (Idx_Hi_R));
                                          Emit_Inst (Ctx.E, En64 & " = add.i64 " & Lo64 & ", " & Img (Long_Long_Integer (Count - 1)));
                                          if Ctx.Enable_Checks and then Count > 0 then
                                             declare
                                                Oob : constant String := Next_Reg (Ctx.E, "agoob");
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

                        for D_I in 1 .. ND loop
                           declare
                              C6 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (Firsts (D_I)), To_String (Lasts (D_I)));
                           begin
                              if D_I = 1 then
                                 Tot64 := To_Unbounded_String (C6);
                              else
                                 declare
                                    M : constant String := Next_Reg (Ctx.E, "agm");
                                 begin
                                    Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & C6);
                                    Tot64 := To_Unbounded_String (M);
                                 end;
                              end if;
                           end;
                        end loop;
                        Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                        Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Byt & ", " & Img (ETI.Align), "ptr", Buf_U);
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
                        Res.Align      := ETI.Align;
                        Res.NDims      := ND;
                        Res.First_Regs := Firsts;
                        Res.Last_Regs  := Lasts;
                        return Res;
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
                           MTI  : constant Type_Info := Ctx.Access_Masters.Element (Tree_Key (Cur));
                           MSym : constant String := To_String (MTI.Pir_Type);
                           MV   : constant String := Next_Reg (Ctx.E, "amv");
                        begin
                           if MSym'Length > 0 and then MSym (MSym'First) = '%' then
                              declare
                                 Sep : Natural := 0;
                              begin
                                 for K in MSym'Range loop
                                    if MSym (K) = '|' then
                                       Sep := K;
                                       exit;
                                    end if;
                                 end loop;
                                 if MTI.Dims (1).First = Integer_64 (Ctx.Cur_Subp_Key) then
                                    declare
                                       Loc_S : constant String :=
                                         (if Sep > 0 then MSym (MSym'First .. Sep - 1) else MSym);
                                    begin
                                       Emit_Inst (Ctx.E, MV & " = load.ptr " & Loc_S & ", align 8");
                                       return To_Unbounded_String (MV);
                                    end;
                                 elsif Sep > 0 and then Sep < MSym'Last then
                                    declare
                                       Glb_S : constant String := MSym (Sep + 1 .. MSym'Last);
                                       MA    : constant String := Next_Reg (Ctx.E, "ama");
                                    begin
                                       Emit_Inst (Ctx.E, MA & " = addr " & Glb_S);
                                       Emit_Inst (Ctx.E, MV & " = load.ptr " & MA & ", align 8");
                                       return To_Unbounded_String (MV);
                                    end;
                                 end if;
                              end;
                           elsif MSym'Length > 0 then
                              declare
                                 MA : constant String := Next_Reg (Ctx.E, "ama");
                              begin
                                 Emit_Inst (Ctx.E, MA & " = addr " & MSym);
                                 Emit_Inst (Ctx.E, MV & " = load.ptr " & MA & ", align 8");
                                 return To_Unbounded_String (MV);
                              end;
                           end if;
                        end;
                     end if;
                     Cur := Resolve_Type_Spec (D (SM_DERIVED, Cur));
                  end loop;
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
                        PSym : constant String := "@ada_pool_used_" & Tree_Key_Str (Cur);
                        UP   : constant String := Next_Reg (Ctx.E, "pup");
                        LV   : constant Expr_Val := Emit_Expr (Ctx, SS_E);
                        L64  : constant String := Emit_Int_Convert (Ctx, To_String (LV.Reg), To_String (LV.Pir_Type), "i64");
                     begin
                        Add_Global_Decl (Ctx.E, PSym, PSym & " = global i64 = 0, align 8");
                        Emit_Inst (Ctx.E, UP & " = addr " & PSym);
                        Emit_Call_Or_Invoke (Ctx, "@__ada_pool_alloc", UP & ", " & L64 & ", " & Sz_I64, "ptr", Out_Ptr);
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

               -- Access-to-unconstrained-array allocator: heap-allocate data + 40-byte @__ada_arr_desc!
               if Is_Access_To_Uncon_Array (ET) then
                  declare
                     Base_Arr : constant TREE := Resolve_Type_Spec (Base_Type_Spec (Des_T));
                     BTI      : constant Type_Info := Get_Type_Info (Ctx, Base_Arr);
                     ETI      : constant Type_Info := Get_Type_Info (Ctx, BTI.Elem_Type);
                     ESz      : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                     Firsts   : Str_Array_4;
                     Lasts    : Str_Array_4;
                     ND       : Natural := 0;
                     Data_Ptr : Unbounded_String;
                     Desc_Ptr : Unbounded_String;
                  begin
                     if K = DN_QUALIFIED_ALLOCATOR then
                        declare
                           Q      : constant TREE := D (AS_QUALIFIED, Node);
                           Init_E : constant TREE := D (AS_EXP, Q);
                           Src    : constant Expr_Val := Emit_Expr (Ctx, Init_E, Alloc_T);
                           Tot64  : Unbounded_String;
                           Byt    : constant String := Next_Reg (Ctx.E, "qab");
                           Nz     : constant String := Next_Reg (Ctx.E, "qanz");
                           AlB    : constant String := Next_Reg (Ctx.E, "qaal");
                        begin
                           ND := (if Src.NDims > 0 then Src.NDims else Natural'Max (1, BTI.NDims));
                           for D_Idx in 1 .. Natural'Min (ND, 4) loop
                              Get_Array_Dim_Bounds (Ctx, Src, D_Idx, Firsts (D_Idx), Lasts (D_Idx));
                              declare
                                 Df : constant String := Next_Reg (Ctx.E, "qad");
                                 Ln : constant String := Next_Reg (Ctx.E, "qal");
                                 Ps : constant String := Next_Reg (Ctx.E, "qap");
                                 Cl : constant String := Next_Reg (Ctx.E, "qac");
                                 C6 : constant String := Next_Reg (Ctx.E, "qac6");
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
                                       M : constant String := Next_Reg (Ctx.E, "qam");
                                    begin
                                       Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & C6);
                                       Tot64 := To_Unbounded_String (M);
                                    end;
                                 end if;
                              end;
                           end loop;
                           Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                           Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                           Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                           Emit_Acc_Alloc (AlB, Data_Ptr);
                           Emit_Inst (Ctx.E, "memcpy " & To_String (Data_Ptr) & ", " & To_String (Src.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
                        end;
                     else
                        -- DN_SUBTYPE_ALLOCATOR for array: evaluate bounds from Alloc_T's SM_INDEX_SUBTYPE_S
                        declare
                           Sub_S : constant TREE :=
                             (if not Is_Void_Tree (Alloc_T) and then KIND (Alloc_T) = DN_CONSTRAINED_ARRAY
                              then D (SM_INDEX_SUBTYPE_S, Alloc_T) else CONST_VOID);
                           Tot64 : Unbounded_String;
                           Byt   : constant String := Next_Reg (Ctx.E, "sab");
                           Nz    : constant String := Next_Reg (Ctx.E, "sanz");
                           AlB   : constant String := Next_Reg (Ctx.E, "saal");
                        begin
                           if not Is_Void_Tree (Sub_S) then
                              declare
                                 IL    : SEQ_TYPE := LIST (Sub_S);
                                 Idx_T : TREE;
                              begin
                                 while not IS_EMPTY (IL) and then ND < 4 loop
                                    POP (IL, Idx_T);
                                    ND := ND + 1;
                                    declare
                                       RT, RL, RH : Unbounded_String;
                                       F32, L32   : Unbounded_String;
                                       Df : constant String := Next_Reg (Ctx.E, "sad");
                                       Ln : constant String := Next_Reg (Ctx.E, "sal");
                                       Ps : constant String := Next_Reg (Ctx.E, "sap");
                                       Cl : constant String := Next_Reg (Ctx.E, "sac");
                                       C6 : constant String := Next_Reg (Ctx.E, "sac6");
                                    begin
                                       Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (Idx_T)), RT, RL, RH);
                                       F32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                                       L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                                       Firsts (ND) := F32;
                                       Lasts (ND)  := L32;
                                       Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L32) & ", " & To_String (F32));
                                       Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                                       Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                                       Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                                       Emit_Inst (Ctx.E, C6 & " = sext.i64 " & Cl);
                                       if ND = 1 then
                                          Tot64 := To_Unbounded_String (C6);
                                       else
                                          declare
                                             M : constant String := Next_Reg (Ctx.E, "sam");
                                          begin
                                             Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & C6);
                                             Tot64 := To_Unbounded_String (M);
                                          end;
                                       end if;
                                    end;
                                 end loop;
                              end;
                           end if;
                           if ND = 0 then
                              ND := 1;
                              Firsts (1) := To_Unbounded_String ("1");
                              Lasts (1)  := To_Unbounded_String ("0");
                              Tot64      := To_Unbounded_String ("0");
                           end if;
                           Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                           Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                           Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                           Emit_Acc_Alloc (AlB, Data_Ptr);
                           -- Default-initialize & activate elements for unconstrained array of tasks/records/access (c85006e!)
                           if Is_Task_Type (BTI.Elem_Type) or else Is_Record_Type (BTI.Elem_Type)
                             or else Is_Access_Type (BTI.Elem_Type)
                             or else (ETI.Kind = TK_Constrained_Array and then not ETI.Is_Dynamic)
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
                                 EP      : constant String := Next_Reg (Ctx.E, "uaep");
                                 Nx      : constant String := Next_Reg (Ctx.E, "uanx");
                                 Dummy   : Unbounded_String;
                              begin
                                 Ctx.Cur_Master_Reg := Mst_R;
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_master_mark", To_String (Mst_R), "ptr", Mk_R, Nounwind => True);
                                 Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (L_Fail),
                                                       Current_Exn   => Null_Unbounded_String,
                                                       In_Handler    => False,
                                                       SS_Mark       => Null_Unbounded_String));
                                 Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                                 Emit_Label (Ctx.E, L_H, IR & ": i64");
                                 Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                                 Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                                 Emit_Label (Ctx.E, L_B);
                                 Emit_Inst (Ctx.E, EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & To_String (Data_Ptr) & ", " & IR);
                                 Emit_Default_Init (Ctx, EP, BTI.Elem_Type, Activate_Tasks => False);
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
                     Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", "40", "ptr", Desc_Ptr);
                     declare
                        P0 : constant String := Next_Reg (Ctx.E, "adp0");
                     begin
                        Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & To_String (Desc_Ptr) & ", 0");
                        Emit_Inst (Ctx.E, "store.ptr " & To_String (Data_Ptr) & ", " & P0 & ", align 8");
                        for D_Idx in 1 .. Natural'Min (ND, 4) loop
                           declare
                              FP : constant String := Next_Reg (Ctx.E, "adfp");
                              LP : constant String := Next_Reg (Ctx.E, "adlp");
                           begin
                              -- Check non-null bounds belong to Base_Arr's index subtype (c48008c/d, c48009d/e)
                              if Ctx.Enable_Checks and then D_Idx <= BTI.NDims and then not Is_Void_Tree (BTI.Dims (D_Idx).Idx_Type) then
                                 declare
                                    NE  : constant String := Next_Reg (Ctx.E, "ane");
                                    L_C : constant String := Next_Label (Ctx.E, "alc");
                                    L_O : constant String := Next_Label (Ctx.E, "alo");
                                    BIT : constant TREE := BTI.Dims (D_Idx).Idx_Type;
                                    BITI : constant Type_Info := Get_Type_Info (Ctx, BIT);
                                 begin
                                    Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (Firsts (D_Idx)) & ", " & To_String (Lasts (D_Idx)));
                                    Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                                    Emit_Label (Ctx.E, L_C);
                                    declare
                                       FC : constant String := Emit_Int_Convert (Ctx, To_String (Firsts (D_Idx)), "i32", To_String (BITI.Pir_Type));
                                       LC : constant String := Emit_Int_Convert (Ctx, To_String (Lasts (D_Idx)), "i32", To_String (BITI.Pir_Type));
                                    begin
                                       Emit_Range_Check (Ctx, FC, To_String (BITI.Pir_Type), BIT);
                                       Emit_Range_Check (Ctx, LC, To_String (BITI.Pir_Type), BIT);
                                    end;
                                    Emit_Term (Ctx.E, "jmp " & L_O);
                                    Emit_Label (Ctx.E, L_O);
                                 end;
                              end if;
                              Emit_Inst (Ctx.E, FP & " = field_ptr @__ada_arr_desc, " & To_String (Desc_Ptr) & ", " & Img (2 * D_Idx - 1));
                              Emit_Inst (Ctx.E, "store.i32 " & To_String (Firsts (D_Idx)) & ", " & FP & ", align 4");
                              Emit_Inst (Ctx.E, LP & " = field_ptr @__ada_arr_desc, " & To_String (Desc_Ptr) & ", " & Img (2 * D_Idx));
                              Emit_Inst (Ctx.E, "store.i32 " & To_String (Lasts (D_Idx)) & ", " & LP & ", align 4");
                           end;
                        end loop;
                        -- Check designated subtype bounds if ET is constrained access-to-array (c48007c, c48008c, c48009d/e/h)
                        if Ctx.Enable_Checks and then not Is_Void_Tree (Des_T) and then KIND (Des_T) = DN_CONSTRAINED_ARRAY then
                           declare
                              DesTI : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                           begin
                              for D_Idx in 1 .. Natural'Min (ND, DesTI.NDims) loop
                                 if not Is_Void_Tree (DesTI.Dims (D_Idx).Idx_Type) then
                                    declare
                                       DF, DL : Unbounded_String;
                                       C1  : constant String := Next_Reg (Ctx.E, "adb1");
                                       C2  : constant String := Next_Reg (Ctx.E, "adb2");
                                       Bad : constant String := Next_Reg (Ctx.E, "adbb");
                                    begin
                                       Emit_Index_Bounds (Ctx, DesTI.Dims (D_Idx).Idx_Type, DF, DL);
                                       Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (Firsts (D_Idx)) & ", " & To_String (DF));
                                       Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (Lasts (D_Idx)) & ", " & To_String (DL));
                                       Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                       Emit_Check_Fail_If (Ctx, Bad);
                                    end;
                                 end if;
                              end loop;
                           end;
                        else
                           Emit_Discriminant_Check (Ctx, To_String (Desc_Ptr), ET, Is_Access => True);
                        end if;
                     end;
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
                           DesTI : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                        begin
                           for D_I in 1 .. Natural'Min (ND_A, DesTI.NDims) loop
                              if not Is_Void_Tree (DesTI.Dims (D_I).Idx_Type) then
                                 declare
                                    DF, DL : Unbounded_String;
                                    C1  : constant String := Next_Reg (Ctx.E, "adb1");
                                    C2  : constant String := Next_Reg (Ctx.E, "adb2");
                                    Bad : constant String := Next_Reg (Ctx.E, "adbb");
                                 begin
                                    Emit_Index_Bounds (Ctx, DesTI.Dims (D_I).Idx_Type, DF, DL);
                                    Emit_Inst (Ctx.E, C1 & " = cmp_ne.i32 " & To_String (AF (D_I)) & ", " & To_String (DF));
                                    Emit_Inst (Ctx.E, C2 & " = cmp_ne.i32 " & To_String (AL (D_I)) & ", " & To_String (DL));
                                    Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                    Emit_Check_Fail_If (Ctx, Bad);
                                 end;
                              end if;
                           end loop;
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
                        ESz    : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                        ND     : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, 4));
                        Firsts : Str_Array_4;
                        Lasts  : Str_Array_4;
                        Tot64  : Unbounded_String;
                        Byt    : constant String := Next_Reg (Ctx.E, "dab");
                        Nz     : constant String := Next_Reg (Ctx.E, "danz");
                        AlB    : constant String := Next_Reg (Ctx.E, "daal");
                        Mst_R  : Unbounded_String := Ctx.Cur_Master_Reg;
                        Mk_R   : Unbounded_String;
                        L_Fail : constant String := Next_Label (Ctx.E, "dalcf");
                        L_Done : constant String := Next_Label (Ctx.E, "dalcd");
                        Ex_R   : constant String := Next_Reg (Ctx.E, "dalce");
                        Dummy  : Unbounded_String;
                     begin
                        for D_I in 1 .. ND loop
                           declare
                              F_R, L_R : Unbounded_String;
                              DL       : Unbounded_String;
                           begin
                              Emit_Index_Bounds (Ctx, DTI.Dims (D_I).Idx_Type, F_R, L_R);
                              Firsts (D_I) := F_R;
                              Lasts (D_I)  := L_R;
                              DL := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_R), To_String (L_R)));
                              if D_I = 1 then
                                 Tot64 := DL;
                              else
                                 declare
                                    M : constant String := Next_Reg (Ctx.E, "dam");
                                 begin
                                    Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & To_String (DL));
                                    Tot64 := To_Unbounded_String (M);
                                 end;
                              end if;
                           end;
                        end loop;
                        Check_Alloc_Arr_Des_Bounds (Firsts, Lasts, ND);
                        Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
                        Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & Byt & ", 0");
                        Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & Byt & ", " & Img (Natural'Max (1, ETI.Align)));
                        Emit_Acc_Alloc (AlB, Ptr_R);
                        if K = DN_QUALIFIED_ALLOCATOR then
                           declare
                              Q      : constant TREE := D (AS_QUALIFIED, Node);
                              Init_E : constant TREE := D (AS_EXP, Q);
                           begin
                              Emit_Init_Composite (Ctx, To_String (Ptr_R), Alloc_T, Init_E,
                                Dst_Firsts => Firsts, Dst_Lasts => Lasts, Exact_Bounds => True);
                           end;
                        elsif Is_Task_Type (DTI.Elem_Type) or else Is_Record_Type (DTI.Elem_Type) or else Is_Access_Type (DTI.Elem_Type) then
                           if Length (Mst_R) = 0 then
                              Emit_Call_Or_Invoke (Ctx, "@__ada_global_master", "", "ptr", Mst_R, Nounwind => True);
                           end if;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_mark", To_String (Mst_R), "ptr", Mk_R, Nounwind => True);
                           Ctx.Handlers.Append ((Handler_Label => To_Unbounded_String (L_Fail),
                                                 Current_Exn   => Null_Unbounded_String,
                                                 In_Handler    => False,
                                                 SS_Mark       => Null_Unbounded_String));
                           declare
                              L_H : constant String := Next_Label (Ctx.E, "dai_h");
                              L_B : constant String := Next_Label (Ctx.E, "dai_b");
                              L_E : constant String := Next_Label (Ctx.E, "dai_e");
                              IR  : constant String := Next_Reg (Ctx.E, "dai");
                              Cnd : constant String := Next_Reg (Ctx.E, "dac");
                              EP  : constant String := Next_Reg (Ctx.E, "daep");
                              Nx  : constant String := Next_Reg (Ctx.E, "danx");
                           begin
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (Tot64));
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & To_String (Ptr_R) & ", " & IR);
                              Emit_Default_Init (Ctx, EP, DTI.Elem_Type, Activate_Tasks => False);
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                           end;
                           Ctx.Handlers.Delete_Last;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_activate_since",
                             To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy);
                           Emit_Term (Ctx.E, "jmp " & L_Done);
                           Emit_Label (Ctx.E, L_Fail, Ex_R & ": ptr");
                           Emit_Call_Or_Invoke (Ctx, "@__ada_master_abort_since",
                             To_String (Mst_R) & ", " & To_String (Mk_R), "", Dummy, Nounwind => True);
                           Emit_Raise_Reg (Ctx, Ex_R);
                           Emit_Label (Ctx.E, L_Done);
                        end if;
                     end;
                  else
                  if DTI.Kind = TK_Constrained_Array then
                     declare
                        ND_S : constant Natural := Natural'Max (1, Natural'Min (DTI.NDims, 4));
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
                        ND_U   : constant Natural := Natural'Max (1, Natural'Min ((if Src.NDims > 0 then Src.NDims else DTI.NDims), 4));
                        AF, AL : Str_Array_4;
                        DesTI  : constant Type_Info := Get_Type_Info (Ctx, Des_T);
                     begin
                        for D_I in 1 .. ND_U loop
                           Get_Array_Dim_Bounds (Ctx, Src, D_I, AF (D_I), AL (D_I));
                        end loop;
                        Check_Alloc_Arr_Des_Bounds (AF, AL, ND_U);
                        Emit_Acc_Alloc (Img (Natural'Max (1, DesTI.Byte_Size)), Ptr_R);
                        Emit_Copy_Composite (Ctx, To_String (Ptr_R), Src, Des_T);
                     end;
                  else
                  Emit_Acc_Alloc (Img (Natural'Max (1, DTI.Byte_Size)), Ptr_R);
                  if K = DN_QUALIFIED_ALLOCATOR then
                     declare
                        Q : constant TREE := D (AS_QUALIFIED, Node);
                        Init_E : constant TREE := D (AS_EXP, Q);
                     begin
                        if DTI.Kind = TK_Scalar then
                           declare
                              V  : constant Expr_Val := Emit_Expr (Ctx, Init_E, Alloc_T);
                              VR : constant String := Emit_Scalar_Coerce (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (DTI.Pir_Type));
                           begin
                              Emit_Range_Check (Ctx, VR, To_String (DTI.Pir_Type), Alloc_T);
                              Emit_Range_Check (Ctx, VR, To_String (DTI.Pir_Type), Des_T);
                              Emit_Discriminant_Check (Ctx, VR, Alloc_T, Is_Access => True);
                              Emit_Discriminant_Check (Ctx, VR, Des_T, Is_Access => True);
                              Emit_Inst (Ctx.E,
                                "store." & To_String (DTI.Pir_Type) & " " & VR &
                                ", " & To_String (Ptr_R) & ", align " & Img (DTI.Align));
                           end;
                        else
                           Emit_Init_Composite (Ctx, To_String (Ptr_R), Alloc_T, Init_E, Exact_Bounds => True);
                           Emit_Discriminant_Check (Ctx, To_String (Ptr_R), Alloc_T);
                           Emit_Discriminant_Check (Ctx, To_String (Ptr_R), ET, Is_Access => True);
                        end if;
                     end;
                  else
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
                                                 SS_Mark       => Null_Unbounded_String));
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
                  if KIND (Def) = DN_TASK_BODY_ID or else KIND (Def) = DN_TYPE_ID then
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
     (Ctx      : in out Context;
      Val_Reg  : String;
      Src_Type : TREE;
      Src_Pir  : String;
      Dst_Type : TREE;
      Dst_Pir  : String) return String
   is
      ST : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Src_Type));
      DT : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Dst_Type));
   begin
      if not Is_Void_Tree (ST) and then not Is_Void_Tree (DT)
        and then Is_Fixed_Type (ST) and then Is_Fixed_Type (DT)
      then
         declare
            Rat : constant IEEE_Float_64 := Fixed_Small (ST) / Fixed_Small (DT);
         begin
            if Rat /= 1.0 then
               declare
                  Fl  : constant String := Next_Reg (Ctx.E, "xxf");
                  Sc  : constant String := Next_Reg (Ctx.E, "xxs");
                  V64 : Unbounded_String;
                  Chk : constant String := (if Ctx.Enable_Checks then "1" else "0");
                  MinB : constant Long_Long_Integer := (if Dst_Pir = "i32" then -2147483648 else Long_Long_Integer'First);
                  MaxB : constant Long_Long_Integer := (if Dst_Pir = "i32" then 2147483647 else Long_Long_Integer'Last);
               begin
                  Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & Val_Reg);
                  Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Rat));
                  Emit_Call_Or_Invoke (Ctx, "@__ada_f64_to_i64_trunc",
                    Sc & ", " & Img (MinB) & ", " & Img (MaxB) & ", " & Chk, "i64", V64);
                  return Emit_Int_Convert (Ctx, To_String (V64), "i64", Dst_Pir);
               end;
            end if;
         end;
      end if;
      if Ctx.Enable_Checks and then not Is_Void_Tree (DT) then
         if (Src_Pir = "i64" or else Src_Pir = "i32" or else Src_Pir = "i16")
           and then (Dst_Pir = "i32" or else Dst_Pir = "i16" or else Dst_Pir = "i8")
           and then Int_Bits_Of_Pir (Src_Pir) > Int_Bits_Of_Pir (Dst_Pir)
           and then Is_Discrete_Type (DT)
         then
            Emit_Range_Check (Ctx, Val_Reg, Src_Pir, DT);
         elsif Src_Pir = "f64" and then Dst_Pir = "f32" and then Is_Float_Type (DT) then
            Emit_Range_Check (Ctx, Val_Reg, "f64", DT);
         end if;
      end if;
      return Emit_Scalar_Coerce (Ctx, Val_Reg, Src_Pir, Dst_Pir);
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
               CS : constant TREE := D (SM_NORMALIZED_DSCRMT_S, R);
               Bound_Discs : U32_Vecs.Vector;
               SD : Saved_Disc_Info;
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
                                    Ctx.Vars.Include (DK, DV);
                                    Bound_Discs.Append (DK);
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
                                       DV64 : constant String := Emit_Int_Convert (Ctx, To_String (DV_Info.Pir_Name), To_String (DV_Info.Pir_Type), "i64");
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
                                                         C64 : constant String := Emit_Int_Convert (Ctx, To_String (CV.Reg), To_String (CV.Pir_Type), "i64");
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
                                                         Lo64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (LoR), To_String (RT2), "i64"));
                                                         Hi64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (HiR), To_String (RT2), "i64"));
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
               for DK of Bound_Discs loop
                  if Ctx.Vars.Contains (DK) then Ctx.Vars.Delete (DK); end if;
               end loop;
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
                              F32, L32 : Unbounded_String;
                              NE  : constant String := Next_Reg (Ctx.E, "tane");
                              L_C : constant String := Next_Label (Ctx.E, "tac");
                              L_O : constant String := Next_Label (Ctx.E, "tao");
                              BIT : constant TREE := BTI.Dims (D_I).Idx_Type;
                              BITI : constant Type_Info := Get_Type_Info (Ctx, BIT);
                           begin
                              if Scalar_Bits (BIT) > 32 and then RTI.Dims (D_I).Is_Static then
                                 -- LRM 3.6.1 (c36202c, c36303a): 64-bit index bounds must not truncate to i32!
                                 declare
                                    F64 : constant String := Next_Reg (Ctx.E, "ta64f");
                                    L64 : constant String := Next_Reg (Ctx.E, "ta64l");
                                 begin
                                    Emit_Inst (Ctx.E, F64 & " = const.i64 " & Img (Long_Long_Integer (RTI.Dims (D_I).First)));
                                    Emit_Inst (Ctx.E, L64 & " = const.i64 " & Img (Long_Long_Integer (RTI.Dims (D_I).Last)));
                                    Emit_Inst (Ctx.E, NE & " = cmp_le_s.i64 " & F64 & ", " & L64);
                                    Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                                    Emit_Label (Ctx.E, L_C);
                                    Emit_Range_Check (Ctx, F64, "i64", BIT);
                                    Emit_Range_Check (Ctx, L64, "i64", BIT);
                                    Emit_Term (Ctx.E, "jmp " & L_O);
                                    Emit_Label (Ctx.E, L_O);
                                 end;
                              else
                              Emit_Index_Bounds (Ctx, RTI.Dims (D_I).Idx_Type, F32, L32);
                              if Save_Allowed and then not RTI.Dims (D_I).Is_Static then
                                 declare
                                    IT_K : constant Unsigned_32 := Tree_Key (Resolve_Type_Spec (Subst_Tree (Ctx, RTI.Dims (D_I).Idx_Type)));
                                    SR   : Saved_Range_Info;
                                 begin
                                    Save_Scalar_Slot (Ctx, To_String (F32), "i32", 4, SR.Lo_Ptr, SR.Is_Global);
                                    Save_Scalar_Slot (Ctx, To_String (L32), "i32", 4, SR.Hi_Ptr, SR.Is_Global);
                                    SR.Pir_Type   := To_Unbounded_String ("i32");
                                    SR.Align      := 4;
                                    SR.Owner_Subp := Ctx.Cur_Subp_Key;
                                    Ctx.Saved_Type_Ranges.Include (IT_K, SR);
                                 end;
                              end if;
                              Emit_Inst (Ctx.E, NE & " = cmp_le_s.i32 " & To_String (F32) & ", " & To_String (L32));
                              Emit_Term (Ctx.E, "br " & NE & ", " & L_C & ", " & L_O);
                              Emit_Label (Ctx.E, L_C);
                              declare
                                 FC : constant String := Emit_Int_Convert (Ctx, To_String (F32), "i32", To_String (BITI.Pir_Type));
                                 LC : constant String := Emit_Int_Convert (Ctx, To_String (L32), "i32", To_String (BITI.Pir_Type));
                              begin
                                 Emit_Range_Check (Ctx, FC, To_String (BITI.Pir_Type), BIT);
                                 Emit_Range_Check (Ctx, LC, To_String (BITI.Pir_Type), BIT);
                              end;
                              Emit_Term (Ctx.E, "jmp " & L_O);
                              Emit_Label (Ctx.E, L_O);
                              end if;
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
               -- Static float/fixed range compatibility vs explicit parent subtype mark (c35003d/f, c35711a/b, c35904a/b)
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
                        R1 : constant String := Emit_Int_Convert (Ctx, To_String (Lo_U), To_String (RT_U), PT);
                        R2 : constant String := Emit_Int_Convert (Ctx, To_String (Hi_U), To_String (RT_U), PT);
                     begin
                        Emit_Inst (Ctx.E, NE & " = cmp_le_s." & PT & " " & R1 & ", " & R2);
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
                 and then (KIND (R) in DN_INTEGER | DN_ENUMERATION
                           or else not (Has_Static_Val (Safe_SM_Value (D (AS_EXP1, Rng)))
                                        and then Has_Static_Val (Safe_SM_Value (D (AS_EXP2, Rng)))))
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
                           Sm : constant IEEE_Float_64 := Fixed_Small (R);
                        begin
                           if To_String (E1.Pir_Type) = "f32" or else To_String (E1.Pir_Type) = "f64" then
                              declare
                                 F64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), "f64");
                                 Dv  : constant String := Next_Reg (Ctx.E, "fxd1");
                                 Iv  : constant String := Next_Reg (Ctx.E, "fxi1");
                              begin
                                 Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & F64 & ", " & Format_Float64 (Sm));
                                 Emit_Inst (Ctx.E, Iv & " = fptosi." & PT & " " & Dv & " [sat]");
                                 R1 := To_Unbounded_String (Iv);
                              end;
                           else
                              R1 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), PT));
                           end if;
                           if To_String (E2.Pir_Type) = "f32" or else To_String (E2.Pir_Type) = "f64" then
                              declare
                                 F64 : constant String := Emit_Scalar_Coerce (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), "f64");
                                 Dv  : constant String := Next_Reg (Ctx.E, "fxd2");
                                 Iv  : constant String := Next_Reg (Ctx.E, "fxi2");
                              begin
                                 Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & F64 & ", " & Format_Float64 (Sm));
                                 Emit_Inst (Ctx.E, Iv & " = fptosi." & PT & " " & Dv & " [sat]");
                                 R2 := To_Unbounded_String (Iv);
                              end;
                           else
                              R2 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), PT));
                           end if;
                        end;
                     else
                        R1 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), PT));
                        R2 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), PT));
                     end if;
                     if PT = "f32" or else PT = "f64" then
                        Emit_Inst (Ctx.E, NE & " = fcmp_ole." & PT & " " & To_String (R1) & ", " & To_String (R2));
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
                                       if not Is_Void_Tree (CT_W) then
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
   begin
      while not Is_Void_Tree (N) and then KIND (N) in DN_PARENTHESIZED | DN_CONVERSION loop
         N := D (AS_EXP, N);
      end loop;
      if not Is_Void_Tree (N) then
         if KIND (N) = DN_USED_OBJECT_ID then
            declare
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (N)));
            begin
               if not Is_Void_Tree (Def) and then Ctx.Vars.Contains (Tree_Key (Def)) then
                  declare
                     VI : constant Var_Info := Ctx.Vars.Element (Tree_Key (Def));
                  begin
                     if VI.Owner_Subp = Ctx.Cur_Subp_Key and then Length (VI.Constrained_Reg) > 0 then
                        return To_String (VI.Constrained_Reg);
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
         elsif KIND (N) = DN_ALL then
            Con := True;
         elsif KIND (N) in DN_SELECTED | DN_INDEXED | DN_SLICE then
            declare
               TS : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, N)));
            begin
               if not Is_Void_Tree (TS) and then KIND (TS) = DN_CONSTRAINED_RECORD then
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
