-- Pass 2 Stage 2 Regression Test:
--   1. Static ** on negative base & >16-bit exponents; static multi-limb MOD
--   2. Exact >64-bit universal_integer folding (10**20 /= 10**20+1, 2**100 / 2**99)
--   3. 135-literal unsigned i8 enumeration: <, >, in, 'Pos, 'Val, 'Succ, 'Pred, 'Image, 1D array <
--   4. Renamed 'Succ/'Pred bounds check & renamed 'Value ignoring return subtype mark
--   5. universal_fixed conversions Integer(A*B) / Float(A/B), exact decimal 'Small, X in F
--   6. Wide null arrays (2 .. Integer'First): not, and, <, 'Length
--   7. Dynamic subtype bounds frozen in X in S and for I in S loop
--   8. Local unconstrained constant String := S does not alias mutable S
--   9. Static constrained-array function return & dynamic constrained-array init length checks
--  10. Boolean array not/xor component subtype check (AI83-00535)
--  11. Multi-choice named array aggregate containing subtype mark (S1 | 4 => 99)
with Text_IO;
procedure Pass2_Stage2_Test is

   procedure Test_Static_Univ_Arith is
      P1 : constant := (-2) ** 2;
      P2 : constant := (-2) ** 3;
      P3 : constant := 1 ** 40_000;
      P4 : constant := (-1) ** 40_001;
      R1 : constant := (-2.0) ** 2;
      M1 : constant := (-1) mod 100_000;
      M2 : constant := (-100_000) mod 100_001;
      Eq : constant Boolean := (100_000_000_000_000_000_000 = 100_000_000_000_000_000_001);
      Q  : Integer := Integer (2 ** 100 / 2 ** 99);
   begin
      if P1 /= 4 or else P2 /= -8 or else P3 /= 1 or else P4 /= -1 then
         raise Program_Error;
      end if;
      if Float (R1) /= 4.0 then
         raise Program_Error;
      end if;
      if M1 /= 99_999 or else M2 /= 1 then
         raise Program_Error;
      end if;
      if Eq or else Q /= 2 then
         raise Program_Error;
      end if;
   end Test_Static_Univ_Arith;

   procedure Test_Unsigned_Enum_And_Renamed_Attrs is
      type Big_E is (
         E000, E001, E002, E003, E004, E005, E006, E007, E008, E009,
         E010, E011, E012, E013, E014, E015, E016, E017, E018, E019,
         E020, E021, E022, E023, E024, E025, E026, E027, E028, E029,
         E030, E031, E032, E033, E034, E035, E036, E037, E038, E039,
         E040, E041, E042, E043, E044, E045, E046, E047, E048, E049,
         E050, E051, E052, E053, E054, E055, E056, E057, E058, E059,
         E060, E061, E062, E063, E064, E065, E066, E067, E068, E069,
         E070, E071, E072, E073, E074, E075, E076, E077, E078, E079,
         E080, E081, E082, E083, E084, E085, E086, E087, E088, E089,
         E090, E091, E092, E093, E094, E095, E096, E097, E098, E099,
         E100, E101, E102, E103, E104, E105, E106, E107, E108, E109,
         E110, E111, E112, E113, E114, E115, E116, E117, E118, E119,
         E120, E121, E122, E123, E124, E125, E126, E127, E128, E129,
         E130, E131, E132, E133, E134);
      type EA is array (Positive range <>) of Big_E;
      A : Big_E := E130;
      B : Big_E := E000;
      X : EA (1 .. 1) := (1 => E130);
      Y : EA (1 .. 1) := (1 => E000);

      subtype Small_Int is Integer range 1 .. 10;
      function Next_E (C : Big_E) return Big_E renames Big_E'Succ;
      function Parse_I (S : String) return Small_Int renames Integer'Value;
      Got_CE : Boolean := False;
      N : Integer;
   begin
      if A < B or else not (A > B) then raise Program_Error; end if;
      if X < Y or else not (X > Y) then raise Program_Error; end if;
      if not (A in E000 .. E134) then raise Program_Error; end if;
      if Big_E'Pos (A) /= 130 then raise Program_Error; end if;
      if Big_E'Val (130) /= E130 then raise Program_Error; end if;
      if Big_E'Succ (E127) /= E128 or else Big_E'Pred (E128) /= E127 then
         raise Program_Error;
      end if;
      if Big_E'Image (A) /= "E130" then raise Program_Error; end if;

      begin
         A := Next_E (E134);
      exception
         when Constraint_Error =>
            Got_CE := True;
      end;
      if not Got_CE then raise Program_Error; end if;

      -- LRM 8.5(7): renamed 'Value ignores Small_Int return subtype mark
      N := Parse_I ("100");
      if N /= 100 then raise Program_Error; end if;
   end Test_Unsigned_Enum_And_Renamed_Attrs;

   procedure Test_Fixed_And_Univ_Fixed is
      type Fix is delta 0.25 range -10.0 .. 10.0;
      A : Fix := 3.0;
      B : Fix := 2.0;
      I : Integer := Integer (A * B);
      R : Float   := Float (A / B);

      type Cent is delta 0.01 range -100.0 .. 100.0;
      for Cent'Small use 0.01;
      C1 : Cent := 0.01;
      C2 : Cent := 0.05;
      Q  : Cent := Cent (C1 / C2);

      type F8 is delta 0.125 range -1.0 .. 1.0;
      X  : F8 := -0.5;
   begin
      if I /= 6 or else R /= 1.5 then raise Program_Error; end if;
      if Q /= 0.20 then raise Program_Error; end if;
      if not (X in F8) then raise Program_Error; end if;
   end Test_Fixed_And_Univ_Fixed;

   procedure Test_Wide_Null_Arrays is
      type BA is array (Integer range <>) of Boolean;
      A : BA (2 .. Integer'First);
      B : BA (2 .. Integer'First);
      C : constant BA := not A;
      D : constant BA := A and B;
      Lt : Boolean := (A < B);
      S : String (2 .. Integer'First);
   begin
      if A'Length /= 0 or else C'Length /= 0 or else D'Length /= 0 then
         raise Program_Error;
      end if;
      if Lt then raise Program_Error; end if;
      if S'Length /= 0 then raise Program_Error; end if;
   end Test_Wide_Null_Arrays;

   procedure Test_Dynamic_Bounds_And_Copies is
      N : Integer := 5;
      subtype S_Dyn is Integer range 1 .. N;
      Sum : Integer := 0;
      Orig : String (1 .. 4) := "ABCD";
      Copy : constant String := Orig;

      subtype Str3 is String (1 .. 3);
      subtype Only_True is Boolean range True .. True;
      type TA is array (1 .. 2) of Only_True;
      T_Arr : TA := (True, True);

      subtype S1 is Integer range 1 .. 2;
      type IA is array (1 .. 5) of Integer;
      M_Arr : IA := (S1 | 4 => 99, 3 | 5 => 7);

      Got_Ret_CE  : Boolean := False;
      Got_Dyn_CE  : Boolean := False;
      Got_Bool_CE : Boolean := False;

      function Bad_Ret (S : String) return Str3 is
      begin
         return S;
      end Bad_Ret;
   begin
      N := 10;
      -- S_Dyn must stay 1 .. 5, NOT 1 .. 10!
      if 7 in S_Dyn then raise Program_Error; end if;
      for I in S_Dyn loop
         Sum := Sum + I;
      end loop;
      if Sum /= 15 then raise Program_Error; end if;

      -- Mutating Orig must NOT mutate constant Copy!
      Orig (1) := 'Z';
      if Copy /= "ABCD" then raise Program_Error; end if;

      -- Static constrained array return must check length
      begin
         declare
            R : Str3 := Bad_Ret ("ABCDEF");
         begin
            if R'Length > 0 then null; end if;
         end;
      exception
         when Constraint_Error =>
            Got_Ret_CE := True;
      end;
      if not Got_Ret_CE then raise Program_Error; end if;

      -- Dynamic constrained array init must check length
      begin
         declare
            K : Integer := 3;
            S_K : String (1 .. K) := Orig; -- length 4 /= 3
         begin
            if S_K'Length > 0 then null; end if;
         end;
      exception
         when Constraint_Error =>
            Got_Dyn_CE := True;
      end;
      if not Got_Dyn_CE then raise Program_Error; end if;

      -- Boolean array not on Only_True must raise Constraint_Error (AI83-00535)
      begin
         if (not T_Arr) = (not T_Arr) then null; end if;
      exception
         when Constraint_Error =>
            Got_Bool_CE := True;
      end;
      if not Got_Bool_CE then raise Program_Error; end if;

      -- Multi-choice aggregate with subtype mark (S1 | 4 => 99, others => 7)
      if M_Arr (1) /= 99 or else M_Arr (2) /= 99 or else M_Arr (3) /= 7
        or else M_Arr (4) /= 99 or else M_Arr (5) /= 7
      then
         raise Program_Error;
      end if;
   end Test_Dynamic_Bounds_And_Copies;

begin
   Test_Static_Univ_Arith;
   Test_Unsigned_Enum_And_Renamed_Attrs;
   Test_Fixed_And_Univ_Fixed;
   Test_Wide_Null_Arrays;
   Test_Dynamic_Bounds_And_Copies;
   Text_IO.Put_Line ("PASS2_STAGE2_OK");
end Pass2_Stage2_Test;
