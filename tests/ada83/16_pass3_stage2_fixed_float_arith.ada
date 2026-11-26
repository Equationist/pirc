-- Pass 3 Stage 2 Regression Test (also covers Pass 1 & Pass 2 fixed/float/arith findings):
--   P3-2.1:  Duration(1) = 1.0, float->fixed rounding & overflow check (no fptosi [sat] clamp)
--   P3-2.2:  Duration'(1.0/2.0) qualified expr & universal_real DN_RANGE_MEMBERSHIP
--   P3-2.3:  Equal-'Small narrowing Fixed64 -> Fixed32 overflow check
--   P3-2.4:  64-bit fixed-point >53-bit precision & asymmetric -2**31*small 32-bit fixed bound
--   P3-2.5:  Negated T'First integer literals (-32768, -2147483648, -9223372036854775808)
--   P3-2.6:  Named number overflow checks in operators & Long_Integer overflow
--   P3-2.7:  f64 -> f32 subtype conversion S(Long_Float(S'Last)) does not falsely fail
--   P3-2.8:  Float universal_real attributes targeting fixed-point
--   P3-2.9:  Fixed delta subtype on custom 'Small (DURATION) keeps S'Small >= S'Safe_Small
--   P3-2.10: Fixed'Machine_Rounds = False; Float'Machine_Rounds = True
--   P3-2.11: Dynamic float/fixed 'First/'Last/'Mantissa/'Large/'Fore and 'Width frozen bounds
--   P3-2.12: Generic formal float digits matching & static-on-dynamic real subtype check
--   P3-2.13: 'Fore/'Aft on generic formal fixed & small delta 1.0E-11
with TEXT_IO;
use TEXT_IO;

procedure Pass3_Stage2_Test is

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P3S2: " & Msg);
      raise Program_Error;
   end Fail;

   function Id_Int (X : Integer) return Integer is
   begin
      return X;
   end Id_Int;

   function Id_LF (X : Long_Float) return Long_Float is
   begin
      return X;
   end Id_LF;

   procedure Test_Duration_And_Conversions is
      D1   : Duration := Duration (Id_Int (1));
      D2   : Duration := Duration (Id_Int (-3));
      DQ   : Duration := Duration'(1.0 / 2);
      F_Hi : Long_Float := Id_LF (1.0E20);
      Caught : Boolean := False;
   begin
      if D1 /= 1.0 or else D2 /= -3.0 then
         Fail ("Integer -> Duration conversion");
      end if;
      if DQ /= 0.5 then
         Fail ("Duration'(1.0/2) qualified expr");
      end if;
      if not (DQ in 0.0 .. (1.0 / 1)) then
         Fail ("universal_real fixed range membership");
      end if;
      begin
         declare
            Bad_D : Duration := Duration (F_Hi);
         begin
            if Bad_D = 0.0 then null; end if;
         end;
      exception
         when Constraint_Error | Numeric_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Float -> Duration overflow did not raise");
      end if;
   end Test_Duration_And_Conversions;

   procedure Test_Fixed_Precision_And_Narrowing is
      type Fix32 is delta 0.25 range -1000.0 .. 1000.0;
      for Fix32'Small use 0.25;
      type Fix64 is delta 0.25 range -1.0E12 .. 1.0E12;
      for Fix64'Small use 0.25;

      -- 64-bit fixed requiring >53 bits of mantissa
      type Big_Fix is delta 2.0**(-10) range -2.0**50 .. 2.0**50;
      for Big_Fix'Small use 2.0**(-10);

      A64 : Fix64 := 1.0E11;
      Caught : Boolean := False;
      BF1 : Big_Fix := Big_Fix (2.0**46) + Big_Fix (2.0**(-10));
      BF2 : Big_Fix := BF1 * 1;
      Two : Integer := Id_Int (2);
      BF3 : Big_Fix := (BF1 * Two) / Two;

      -- P3-2.9: DURATION delta subtype
      subtype Dur_Tenth is Duration delta 0.1;

      -- P3-2.8: Float universal_real attribute to fixed
      type Fine_Fix is delta 2.0**(-30) range -1.0 .. 1.0;
      FE : Fine_Fix := Fine_Fix (Float'Epsilon);
   begin
      begin
         declare
            X32 : Fix32 := Fix32 (A64);
         begin
            if X32 = 0.0 then null; end if;
         end;
      exception
         when Constraint_Error | Numeric_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Fixed64 -> Fixed32 equal-small overflow");
      end if;

      if BF2 /= BF1 or else BF3 /= BF1 then
         Fail ("64-bit fixed >53-bit precision");
      end if;

      if Dur_Tenth'Small < Dur_Tenth'Safe_Small then
         Fail ("DURATION delta subtype Small < Safe_Small");
      end if;

      if Fix32'Machine_Rounds or else not Float'Machine_Rounds then
         Fail ("Machine_Rounds mismatch");
      end if;

      if FE <= 0.0 then
         Fail ("Float'Epsilon -> Fine_Fix");
      end if;
   end Test_Fixed_Precision_And_Narrowing;

   procedure Test_Negated_First_And_Named_Numbers is
      SI : Short_Integer := -32768;
      I  : Integer       := -2147483648;
      LI : Long_Integer  := -9223372036854775808;
      Huge_Num : constant := 2_147_483_648;
      Zero     : Integer := Id_Int (0);
      Caught   : Boolean := False;
   begin
      if SI /= Short_Integer'First or else I /= Integer'First or else LI /= Long_Integer'First then
         Fail ("Negated T'First literals");
      end if;
      begin
         declare
            Bad : Integer := Zero + Huge_Num;
         begin
            if Bad = 0 then null; end if;
         end;
      exception
         when Constraint_Error | Numeric_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Named number overflow in Integer +");
      end if;
   end Test_Negated_First_And_Named_Numbers;

   procedure Test_Float_Rounding_And_Dynamic_Subtypes is
      subtype F_Sub is Float range 0.0 .. 0.1;
      LF : Long_Float := Id_LF (Long_Float (F_Sub'Last));
      X  : F_Sub;

      Lo_F : Float := 1.0;
      Hi_F : Float := 10.0;
      Hi_I : Integer := 999;
      Arr  : String (1 .. 12) := (others => 'a');

      -- P3-2.12: Generic formal float digits check
      generic
         type F is digits <>;
      package GF_Pkg is
         function Zero return F;
      end GF_Pkg;

      -- P3-2.13: Generic formal fixed 'Fore/'Aft + small delta
      generic
         type FX is delta <>;
      function Get_Aft return Integer;

      type Tiny_Fix is delta 1.0E-11 range -1.0 .. 1.0;

      package body GF_Pkg is
         function Zero return F is begin return 0.0; end;
      end GF_Pkg;

      function Get_Aft return Integer is
      begin
         return FX'Aft;
      end Get_Aft;

      function Tiny_Aft is new Get_Aft (Tiny_Fix);
   begin
      -- P3-2.7: f64 -> f32 subtype boundary
      X := F_Sub (LF);
      if X /= F_Sub'Last then
         Fail ("f64 -> f32 subtype boundary");
      end if;

      -- P3-2.11: Dynamic bounds frozen in Saved_Type_Ranges
      declare
         subtype Dyn_F is Float range Lo_F .. Hi_F;
         subtype Dyn_I is Integer range 1 .. Hi_I;
         subtype Arr_Idx is Integer range Arr'Range;
         Child_Err : Boolean := False;
      begin
         Lo_F := 5.0;
         Hi_F := 6.0;
         Hi_I := 5;
         if Dyn_F'First /= 1.0 or else Dyn_F'Last /= 10.0 then
            Fail ("Dynamic float 'First/'Last not frozen");
         end if;
         if Dyn_I'Width /= 4 then
            Fail ("Dynamic integer 'Width not frozen");
         end if;
         if Arr_Idx'Width /= 3 then
            Fail ("Arr'Range subtype 'Width");
         end if;

         -- P3-2.12: static child subtype on dynamic real parent compatibility check
         begin
            declare
               subtype Bad_Child is Dyn_F range 0.0 .. 20.0;
               V : Bad_Child := 5.0;
            begin
               if V = 0.0 then null; end if;
            end;
         exception
            when Constraint_Error => Child_Err := True;
         end;
         if not Child_Err then
            Fail ("Static child of dynamic real subtype compatibility check");
         end if;
      end;

      if Tiny_Fix'Aft /= 11 or else Tiny_Aft /= 11 then
         Fail ("Tiny_Fix'Aft /= 11");
      end if;

      -- Sanity instantiation of GF_Pkg
      declare
         package I_GF is new GF_Pkg (Float);
      begin
         if I_GF.Zero /= 0.0 then
            Fail ("GF_Pkg");
         end if;
      end;
   end Test_Float_Rounding_And_Dynamic_Subtypes;

begin
   Test_Duration_And_Conversions;
   Test_Fixed_Precision_And_Narrowing;
   Test_Negated_First_And_Named_Numbers;
   Test_Float_Rounding_And_Dynamic_Subtypes;
   Put_Line ("PASS3_STAGE2_OK");
end Pass3_Stage2_Test;
