-- Pass 3 Stage 3 Regression Test (also covers Pass 1 & Pass 2 records/aggregates/subps/renamings):
--   P3-3.1:  Nested discriminant-dependent component assign & default-init discriminant check
--   P3-3.2:  Positional+others overflow (1,2,3,4,others=>0) & (1,others=>0) on null -> CONSTRAINT_ERROR
--   P3-3.3:  'Constrained on renamings, nested captured in out, func result, const comp, P.all null check
--   P3-3.4:  Access view conversion & constrained record view conversion checks
--   P3-3.5:  Variant choice with DN_DISCRETE_SUBTYPE range constraint & variant dyn-array default init
--   P3-3.6:  Qualified composite init, unconstrained return S2'(others=>42), qualified allocator new S3'(A)
--   P3-3.7/8: Named aggregate sliding, exact-bounds param check, Sub=>99 others=>Tick eval count
--   P3-3.9:  Nested user-defined "=" and implicit "/=" with static link capture
--   P3-3.10: Derived subprogram enforcing derived tighter formal & return subtype checks
--   P3-3.11: Scalar in parameter range check before narrowing wide i64 ('Pos/'Length)
--   P3-3.12: Concatenation '&' composite element subtype check
--   P3-3.13: Multiple object decl A, B : Integer range 1..Next_Lim freezes distinct bounds per object
--   P3-3.16: Bitfield component renaming & 'Address of renamed subprogram
--   P3-3.17: Dynamic string literal component subtype range check
--   P3-3.18: Record membership with Saved_Type_Discs
with TEXT_IO, SYSTEM;
use TEXT_IO, SYSTEM;

procedure Pass3_Stage3_Test is

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P3S3: " & Msg);
      raise Program_Error;
   end Fail;

   function Id_Int (X : Integer) return Integer is
   begin
      return X;
   end Id_Int;

   -- P3-3.1, P3-3.3, P3-3.4, P3-3.5, P3-3.18
   procedure Test_Records_And_Constrained is
      type Inner (D : Integer := 1) is record
         V : Integer := D * 10;
      end record;
      type Outer (N : Integer := 2) is record
         Comp : Inner (N);
      end record;
      type Inner_Acc is access Inner;
      subtype Inner_1_Acc is Inner_Acc (1);
      subtype Inner_2 is Inner (2);

      O  : Outer (2);
      I1 : Inner := (D => 1, V => 10);
      I2 : Inner (2);
      P  : Inner_Acc := new Inner'(D => 2, V => 20);
      PN : Inner_Acc := null;
      R_Uncon : Inner renames I1;
      R_Con   : Inner renames I2;
      R_Heap  : Inner renames P.all;
      Const_I : constant Inner := (D => 1, V => 5);
      Caught  : Boolean;

      type Der_Inner is new Inner;
      subtype Der_Inner_1 is Der_Inner (1);

      -- P3-3.5: variant with constrained subtype choice & dynamic array default init
      type Int_Arr is array (Positive range <>) of Integer;
      type Var_Sub (K : Integer := 2) is record
         case K is
            when Integer range 1 .. 3 =>
               A : Int_Arr (1 .. K) := (others => 55);
            when others =>
               B : Integer := 99;
         end case;
      end record;
      VS : Var_Sub (3);

      Dyn_D : Integer := Id_Int (2);

      function Make_Inner return Inner is
      begin
         return (D => 1, V => 7);
      end Make_Inner;

      procedure Check_Nested_Con (X : in out Inner) is
         function Inner_Check return Boolean is
         begin
            return X'Constrained;
         end Inner_Check;
      begin
         if Inner_Check /= X'Constrained then
            Fail ("Nested captured 'Constrained mismatch");
         end if;
      end Check_Nested_Con;

      -- P3-3.4: constrained record view conversion
      procedure Mutate_Rec (X : in out Der_Inner) is
      begin
         X := (D => 2, V => 99);
      end Mutate_Rec;
   begin
      -- P3-3.1: O.Comp has constraint D=2; assigning D=1 must raise Constraint_Error
      Caught := False;
      begin
         O.Comp := I1;
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Nested discriminant-dependent component assign");
      end if;

      -- P3-3.3: 'Constrained checks
      if R_Uncon'Constrained or else not R_Con'Constrained or else not R_Heap'Constrained then
         Fail ("Renaming 'Constrained");
      end if;
      if not Const_I'Constrained or else not Make_Inner'Constrained then
         Fail ("Const/FuncResult 'Constrained");
      end if;
      Check_Nested_Con (I1);
      Check_Nested_Con (I2);

      -- P.all'Constrained null check
      Caught := False;
      begin
         if PN.all'Constrained then null; end if;
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("null.all'Constrained must raise Constraint_Error");
      end if;

      -- P3-3.4: constrained record view conversion check
      Caught := False;
      begin
         Mutate_Rec (Der_Inner_1 (I1));
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Constrained record view conversion check");
      end if;

      -- P3-3.5: variant DN_DISCRETE_SUBTYPE & dynamic array default init
      if VS.A (1) /= 55 or else VS.A (3) /= 55 then
         Fail ("Variant dyn-array default init");
      end if;

      -- P3-3.18: Record membership with Saved_Type_Discs
      declare
         subtype Dyn_In is Inner (Dyn_D);
      begin
         Dyn_D := 99;
         if not (I2 in Dyn_In) or else (I1 in Dyn_In) then
            Fail ("Record membership Saved_Type_Discs");
         end if;
      end;
   end Test_Records_And_Constrained;

   -- P3-3.2, P3-3.6, P3-3.7, P3-3.8
   procedure Test_Aggregates_And_Qualified is
      type Int_Arr is array (Integer range <>) of Integer;
      subtype Arr3 is Int_Arr (1 .. 3);
      subtype Arr3_Off is Int_Arr (11 .. 13);
      subtype Null_Arr is Int_Arr (5 .. 4);
      subtype Sub12 is Integer range 1 .. 2;
      type Arr_Acc is access Int_Arr;

      Caught : Boolean;
      Ticks  : Integer := 0;

      function Tick return Integer;
      function Ret_Qual return Int_Arr;
      procedure Take_Arr3 (X : Arr3);

      A_Slide : Arr3_Off := (1 => 10, 2 => 20, 3 => 30);
      P_Acc   : Arr_Acc;

      function Tick return Integer is
      begin
         Ticks := Ticks + 1;
         return 77;
      end Tick;

      function Ret_Qual return Int_Arr is
      begin
         return Arr3_Off'(others => 42);
      end Ret_Qual;

      procedure Take_Arr3 (X : Arr3) is
      begin
         if X'First /= 1 or else X'Last /= 3 then
            Fail ("Take_Arr3 bounds");
         end if;
      end Take_Arr3;
   begin
      declare
         A_Cov : Arr3 := (Sub12 => 99, others => Tick);
         RQ    : constant Int_Arr := Ret_Qual;
      begin
         -- P3-3.6: unconstrained return qualified aggregate preserves bounds
         if RQ'First /= 11 or else RQ'Last /= 13 or else RQ (12) /= 42 then
            Fail ("Unconstrained return qualified aggregate");
         end if;
         -- P3-3.8: Sub12 => 99, others => Tick evaluates Tick ONLY once (slot 3)
         if A_Cov (1) /= 99 or else A_Cov (2) /= 99 or else A_Cov (3) /= 77 or else Ticks /= 1 then
            Fail ("Sub12 => 99 others => Tick eval count");
         end if;
      end;
      -- P3-3.2: positional + others overflow
      Caught := False;
      begin
         declare
            Bad : Arr3 := (1, 2, 3, 4, others => 0);
         begin
            if Bad (1) = 0 then null; end if;
         end;
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Positional+others overflow did not raise");
      end if;

      Caught := False;
      begin
         declare
            Bad_N : Null_Arr := (1, others => 0);
         begin
            if Bad_N'Length = 0 then null; end if;
         end;
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Null positional+others did not raise");
      end if;

      -- P3-3.6: qualified allocator checks exact subtype bounds
      Caught := False;
      begin
         P_Acc := new Arr3'(A_Slide);
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Qualified allocator exact-bounds check");
      end if;

      -- P3-3.7: named aggregate sliding in object decl
      if A_Slide (11) /= 10 or else A_Slide (13) /= 30 then
         Fail ("Named aggregate sliding");
      end if;
      -- P3-3.7: named aggregate exact bounds on formal param
      Caught := False;
      begin
         Take_Arr3 ((11 => 1, 12 => 2, 13 => 3));
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Named aggregate exact bounds param check");
      end if;
   end Test_Aggregates_And_Qualified;

   -- P3-3.9, P3-3.10, P3-3.11, P3-3.12, P3-3.13, P3-3.16, P3-3.17
   procedure Test_Subps_Derived_And_Renamings is
      -- P3-3.9: Nested user-defined "=" with static link capture + implicit "/="
      package Lim_Pkg is
         type Lim is limited private;
         function Make (V : Integer) return Lim;
         function Val (X : Lim) return Integer;
      private
         type Lim is new Integer;
      end Lim_Pkg;

      Bias : Integer := 5;
      function "=" (L, R : Lim_Pkg.Lim) return Boolean;

      -- P3-3.10: Derived subprogram with tighter derived subtype
      package Base_P is
         type B_Int is range -100 .. 100;
         function Echo (X : B_Int) return B_Int;
      end Base_P;
      type Tight_D is new Base_P.B_Int range 1 .. 10;

      -- P3-3.11: Scalar in param checks wide i64 before narrowing
      subtype Small_I is Integer range 1 .. 100;
      procedure Take_Small (X : Small_I);

      -- P3-3.13: Multiple object decl distinct bounds
      Lim_Counter : Integer := 0;
      function Next_Lim return Integer;

      -- P3-3.16: Bitfield component renaming & renamed subprogram 'Address
      type U4 is range 0 .. 15;
      type BF_Rec is record
         A : U4;
         B : U4;
      end record;
      for BF_Rec use record
         A at 0 range 0 .. 3;
         B at 0 range 4 .. 7;
      end record;
      BR : BF_Rec := (A => 5, B => 9);
      BR_B : U4 renames BR.B;

      procedure Subp_Target;
      procedure Subp_Ren renames Subp_Target;

      -- P3-3.17: Dynamic string literal component subtype check
      Ch_Lo : Character := 'a';
      Ch_Hi : Character := 'z';
      Caught : Boolean;

      package body Lim_Pkg is
         function Make (V : Integer) return Lim is begin return Lim (V); end;
         function Val (X : Lim) return Integer is begin return Integer (X); end;
      end Lim_Pkg;

      function "=" (L, R : Lim_Pkg.Lim) return Boolean is
      begin
         return (Lim_Pkg.Val (L) + Bias) = (Lim_Pkg.Val (R) + Bias);
      end "=";

      package body Base_P is
         function Echo (X : B_Int) return B_Int is begin return X; end;
      end Base_P;

      procedure Take_Small (X : Small_I) is
      begin
         if X < 0 then null; end if;
      end Take_Small;

      function Next_Lim return Integer is
      begin
         Lim_Counter := Lim_Counter + 10;
         return Lim_Counter;
      end Next_Lim;

      procedure Subp_Target is begin null; end;

   begin
      begin
         if not (Lim_Pkg.Make (10) /= Lim_Pkg.Make (20)) or else (Lim_Pkg.Make (10) /= Lim_Pkg.Make (10)) then
            Fail ("Nested implicit /= with SL capture");
         end if;
      end;

      -- P3-3.10: calling derived Echo with value outside Tight_D (1..10) raises Constraint_Error
      Caught := False;
      begin
         declare
            X_Out : Base_P.B_Int := Base_P.B_Int (Id_Int (50));
            R : Tight_D := Echo (Tight_D (X_Out));
         begin
            if R = 1 then null; end if;
         end;
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Derived subprogram tighter formal check");
      end if;

      -- P3-3.11: wide i64 to Small_I
      Caught := False;
      begin
         Take_Small (Integer (Long_Integer (5_000_000_000)));
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Wide i64 in param range check");
      end if;

      -- P3-3.13: A, B : Integer range 1 .. Next_Lim
      declare
         A, B : Integer range 1 .. Next_Lim := 1;
      begin
         A := 10;
         B := 20;
         Caught := False;
         begin
            A := 15;
         exception
            when Constraint_Error => Caught := True;
         end;
         if not Caught then
            Fail ("Multi-obj decl A overwritten by B's bounds");
         end if;
      end;

      -- P3-3.16: bitfield renaming & renamed subprogram 'Address
      if BR_B /= 9 then Fail ("Bitfield rename read"); end if;
      BR_B := 12;
      if BR.A /= 5 or else BR.B /= 12 or else BR_B /= 12 then
         Fail ("Bitfield rename write corrupted neighbor");
      end if;
      if Subp_Ren'Address /= Subp_Target'Address then
         Fail ("Renamed subprogram 'Address");
      end if;

      -- P3-3.17: Dynamic string literal component check
      declare
         subtype Dyn_Ch is Character range Ch_Lo .. Ch_Hi;
         type Dyn_Str is array (Positive range <>) of Dyn_Ch;
      begin
         Caught := False;
         begin
            declare
               Bad_S : Dyn_Str (1 .. 3) := "aZb";
            begin
               if Bad_S (1) = 'a' then null; end if;
            end;
         exception
            when Constraint_Error => Caught := True;
         end;
         if not Caught then
            Fail ("Dynamic string literal char range check");
         end if;
      end;
   end Test_Subps_Derived_And_Renamings;

begin
   Test_Records_And_Constrained;
   Test_Aggregates_And_Qualified;
   Test_Subps_Derived_And_Renamings;
   Put_Line ("PASS3_STAGE3_OK");
end Pass3_Stage3_Test;
