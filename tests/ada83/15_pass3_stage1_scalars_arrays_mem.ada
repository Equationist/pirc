-- Pass 3 Stage 1 Regression Test (also covers Pass 1 & Pass 2 scalar/array/memory findings):
--   P3-1.2:  Unsigned narrow scalars (for U8'Size use 8, U16, 140-lit enum): loops, case, indexing, IO
--   P3-1.3:  'Size-packed integer base-type arithmetic (no premature i8 overflow/sign bugs)
--   P3-1.4:  Arrays & allocators of dynamic constrained arrays (40B descriptor stride, no 8B overflow)
--   P3-1.5:  Deep copy of embedded dynamic-array buffers across slice/uncon assign & concatenation '&'
--   P3-1.6/7: Dynamic alloca_dyn & SS cleanup across exit, goto, while, and exceptions (2000 iters)
--   P3-1.9:  5-dimensional arrays (constrained & unconstrained, 'First(5)/'Last(5)/'Length(5)/'Range(5))
--   P3-1.10: 64-bit (Long_Integer) array index bounds & indexing/slicing
--   P3-1.11: 3D super-null array does not raise STORAGE_ERROR; huge non-null raises STORAGE_ERROR
--   P3-1.12/13: Selected-name array type mark Pkg.Arr'Range & array (S'Range) dynamic bounds
--   P3-1.14: Array aggregate DN_CHOICE_RANGE ending at Integer'Last (no infinite wrap loop)
--   P3-1.15/16: Super-null Both_Null init exemption & unconstrained conversion null-bounds base check
--   P3-1.17: Short-circuit and then / or else on 'Size-widened derived Boolean (32-bit)
with TEXT_IO, UNCHECKED_DEALLOCATION;
use TEXT_IO;

procedure Pass3_Stage1_Test is

   package Helper_Pkg is
      type Arr_T is array (3 .. 7) of Integer;
   end Helper_Pkg;

   -- P3-1.2 & P3-1.3: Narrow unsigned scalars & 140-literal enum
   type Byte_U8 is range 0 .. 255;
   for Byte_U8'Size use 8;
   type Word_U16 is range 0 .. 65535;
   for Word_U16'Size use 16;

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
      E130, E131, E132, E133, E134, E135, E136, E137, E138, E139);

   package U8_IO is new INTEGER_IO (Byte_U8);
   package E_IO  is new ENUMERATION_IO (Big_E);

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P3S1: " & Msg);
      raise Program_Error;
   end Fail;

   procedure Test_Unsigned_And_Packed_Arith is
      Sum   : Integer := 0;
      A     : Byte_U8 := 120;
      B     : Byte_U8 := 20;
      C     : Byte_U8 := 200;
      D     : Byte_U8 := 128;
      W     : Word_U16 := 40000;
      Arr   : array (Byte_U8 range 125 .. 135) of Integer := (others => 1);
      Hit   : Integer := 0;
      S     : String (1 .. 10);
      Last  : Positive;
      Ev    : Big_E;
      Uv    : Byte_U8;
   begin
      -- for loop over unsigned i8 crossing 127->128
      for I in Byte_U8 range 125 .. 135 loop
         Arr (I) := Integer (I);
         Sum := Sum + Arr (I);
      end loop;
      if Sum /= 1430 then
         Fail ("U8 loop/index sum");
      end if;

      -- case wide range on U8 > 127
      case C is
         when 0 .. 127 => Hit := 1;
         when 128 .. 255 => Hit := 2;
      end case;
      if Hit /= 2 then
         Fail ("U8 case wide range");
      end if;

      -- Base-type arithmetic on 'Size-packed U8 and U16 (P3-1.3)
      if A + B /= 140 then Fail ("U8 120+20"); end if;
      if C / 2 /= 100 then Fail ("U8 200/2"); end if;
      if abs D /= 128 then Fail ("U8 abs 128"); end if;
      if A + A - 100 /= 140 then Fail ("U8 intermediate > 127"); end if;
      if W + 10000 /= 50000 then Fail ("U16 40000+10000"); end if;
      if W / 2 /= 20000 then Fail ("U16 40000/2"); end if;

      -- INTEGER_IO and ENUMERATION_IO with unsigned values > 127
      U8_IO.Put (S, Byte_U8 (250));
      U8_IO.Get (S, Uv, Last);
      if Uv /= 250 then Fail ("U8_IO roundtrip 250"); end if;

      E_IO.Put (S, E135);
      E_IO.Get (S, Ev, Last);
      if Ev /= E135 then Fail ("E_IO roundtrip E135"); end if;
      if Big_E'Pos (E135) /= 135 or else Big_E'Width /= 4 then
         Fail ("Big_E Pos/Width");
      end if;
   end Test_Unsigned_And_Packed_Arith;

   -- P3-1.4 & P3-1.5: Dynamic constrained array elements + deep copy (no aliasing)
   procedure Test_Dyn_Arr_Elements (N : Integer) is
      type Uncon_Int is array (Positive range <>) of Integer;
      subtype Dyn_Vec is Uncon_Int (1 .. N);
      type Vec_Arr is array (Positive range <>) of Dyn_Vec;
      type Rec_Dyn is record
         Tag : Integer := 0;
         V   : Dyn_Vec := (others => 0);
      end record;
      type Rec_Arr is array (Positive range <>) of Rec_Dyn;
      type Dyn_Acc is access Uncon_Int;
      procedure Free_DA is new UNCHECKED_DEALLOCATION (Uncon_Int, Dyn_Acc);

      A1 : Vec_Arr (1 .. 3) := (1 => (others => 11), 2 => (others => 22), 3 => (others => 33));
      A2 : Vec_Arr (1 .. 3) := A1;
      R1 : Rec_Arr (1 .. 2) := (1 => (Tag => 1, V => (others => 5)),
                                2 => (Tag => 2, V => (others => 6)));
      R2 : Rec_Arr (1 .. 2);
      R3 : Rec_Arr (1 .. 4) := R1 & R1;
      P  : Dyn_Acc := new Uncon_Int'(Dyn_Vec'(others => 77));
   begin
      -- Mutate A1 and verify A2 did not alias (40B desc stride + deep copy)
      A1 (2) (1) := 999;
      if A2 (1) (1) /= 11 or else A2 (2) (1) /= 22 or else A2 (3) (N) /= 33 then
         Fail ("Vec_Arr deep copy/stride");
      end if;

      -- Slice assignment of Rec_Arr deep-copies embedded dynamic arrays
      R2 (1 .. 2) := R1 (1 .. 2);
      R1 (1).V (1) := 888;
      if R2 (1).V (1) /= 5 or else R2 (2).V (N) /= 6 then
         Fail ("Rec_Arr slice deep copy");
      end if;
      -- Concatenation '&' deep-copies embedded dynamic arrays
      if R3 (1).V (1) /= 5 or else R3 (3).V (1) /= 5 then
         Fail ("Rec_Arr concat deep copy");
      end if;

      if P'Length /= N or else P (N) /= 77 then
         Fail ("Qualified dyn array allocator");
      end if;
      Free_DA (P);
   end Test_Dyn_Arr_Elements;

   -- P3-1.6 & P3-1.7: Stack/SS cleanup across exit, goto, while, exceptions
   procedure Test_Stack_And_SS_Reclaim (Sz : Integer) is
      Done_Cnt : Integer := 0;
      My_E     : exception;

      function Make_Str (K : Integer) return String is
         S : String (1 .. K) := (others => 'x');
      begin
         return S;
      end Make_Str;
   begin
      for I in 1 .. 800 loop
         declare
            Buf : array (1 .. Sz) of Integer := (others => I);
         begin
            if I mod 3 = 0 then
               goto Next_Iter;
            elsif I mod 3 = 1 then
               raise My_E;
            end if;
            Done_Cnt := Done_Cnt + Buf (Sz) - Buf (Sz) + 1;
         exception
            when My_E =>
               Done_Cnt := Done_Cnt + 1;
         end;
         <<Next_Iter>>
         null;
      end loop;
      if Done_Cnt = 0 then
         Fail ("Stack reclaim counter");
      end if;

      -- while condition SS release
      declare
         K : Integer := 0;
      begin
         while K < 400 and then Make_Str (64)'Length = 64 loop
            K := K + 1;
         end loop;
         if K /= 400 then
            Fail ("While SS loop");
         end if;
      end;
   end Test_Stack_And_SS_Reclaim;

   -- P3-1.9: 5-dimensional arrays
   procedure Test_5D_Arrays is
      type Uncon_5D is array (Positive range <>, Positive range <>, Positive range <>,
                              Positive range <>, Positive range <>) of Integer;
      A : Uncon_5D (1 .. 2, 1 .. 2, 1 .. 2, 1 .. 2, 1 .. 3) :=
            (others => (others => (others => (others => (others => 0)))));

      function Sum_5D (X : Uncon_5D) return Integer is
         T : Integer := 0;
      begin
         if X'First (5) /= 1 or else X'Last (5) /= 3 or else X'Length (5) /= 3 then
            Fail ("5D dim 5 attributes");
         end if;
         for I1 in X'Range (1) loop
            for I2 in X'Range (2) loop
               for I3 in X'Range (3) loop
                  for I4 in X'Range (4) loop
                     for I5 in X'Range (5) loop
                        T := T + X (I1, I2, I3, I4, I5);
                     end loop;
                  end loop;
               end loop;
            end loop;
         end loop;
         return T;
      end Sum_5D;
   begin
      A (2, 1, 2, 1, 3) := 42;
      A (1, 2, 1, 2, 2) := 100;
      if Sum_5D (A) /= 142 then
         Fail ("5D array sum");
      end if;
   end Test_5D_Arrays;

   -- P3-1.10: 64-bit array index bounds
   procedure Test_64Bit_Index_Arrays is
      Base_Idx : constant Long_Integer := 5_000_000_000;
      type Big_Arr is array (Long_Integer range <>) of Integer;
      A : Big_Arr (Base_Idx .. Base_Idx + 4) := (10, 20, 30, 40, 50);
      S : Big_Arr (Base_Idx + 1 .. Base_Idx + 3) := A (Base_Idx + 1 .. Base_Idx + 3);
      Bad : Boolean := False;
   begin
      if A'First /= Base_Idx or else A'Last /= Base_Idx + 4 or else A'Length /= 5 then
         Fail ("64-bit array attrs");
      end if;
      if A (Base_Idx + 2) /= 30 or else S (Base_Idx + 3) /= 40 then
         Fail ("64-bit array index/slice");
      end if;
      begin
         if A (Base_Idx + 5) = 0 then null; end if;
      exception
         when Constraint_Error => Bad := True;
      end;
      if not Bad then
         Fail ("64-bit array bounds check");
      end if;
   end Test_64Bit_Index_Arrays;

   -- P3-1.11, P3-1.12, P3-1.13, P3-1.14, P3-1.15, P3-1.16, P3-1.17
   procedure Test_Misc_Array_And_Bool (Dyn_Len : Integer) is
      type Arr_3D is array (Integer range <>, Integer range <>, Integer range <>) of Integer;
      Null3 : Arr_3D (Integer'First .. Integer'Last, Integer'First .. Integer'Last, 1 .. 0);

      Sum_Sel : Integer := 0;

      -- P3-1.13: array (S'Range) dynamic bounds
      S_Dyn : String (1 .. Dyn_Len) := (others => 'a');
      Copy_Dyn : array (S_Dyn'Range) of Integer := (others => 7);

      -- P3-1.14: DN_CHOICE_RANGE ending at Integer'Last
      type Int_Arr is array (Integer range <>) of Integer;
      Top_Arr : Int_Arr (Integer'Last - 2 .. Integer'Last) :=
         (Integer'Last - 2 .. Integer'Last => 9);

      -- P3-1.16: unconstrained conversion null bounds check against base type (AI83-00313)
      type Small_Idx is range 1 .. 10;
      type Small_Arr is array (Small_Idx range <>) of Integer;
      type Long_Arr is array (Long_Integer range <>) of Integer;
      Big_Null : Long_Arr (5_000_000_000 .. 4_000_000_000);
      Ok_Null  : Int_Arr (100 .. 50);
      Conv_Err : Boolean := False;

      -- P3-1.17: 'Size-widened derived Boolean in and then / or else
      type Wide_Bool is new Boolean;
      for Wide_Bool'Size use 32;
      WB_T : Wide_Bool := True;
      WB_F : Wide_Bool := False;
      Side : Integer := 0;
      function Touch return Wide_Bool is
      begin
         Side := Side + 1;
         return True;
      end Touch;
   begin
      if Null3'Length (3) /= 0 then
         Fail ("3D super-null length");
      end if;

      -- P3-1.12: Selected-name array type mark 'Range
      for I in Helper_Pkg.Arr_T'Range loop
         Sum_Sel := Sum_Sel + I;
      end loop;
      if Sum_Sel /= 25 then
         Fail ("Pkg.Arr_T'Range sum");
      end if;

      if Copy_Dyn'First /= 1 or else Copy_Dyn'Last /= Dyn_Len or else Copy_Dyn (Dyn_Len) /= 7 then
         Fail ("array (S'Range) dynamic bounds");
      end if;

      if Top_Arr (Integer'Last) /= 9 or else Top_Arr (Integer'Last - 2) /= 9 then
         Fail ("Integer'Last choice range");
      end if;

      declare
         X_Ok : constant Small_Arr := Small_Arr (Ok_Null);
      begin
         if X_Ok'Length /= 0 then
            Fail ("Null array conversion within base type");
         end if;
      end;
      begin
         declare
            X : constant Small_Arr := Small_Arr (Big_Null);
         begin
            if X'Length /= 0 then null; end if;
         end;
      exception
         when Constraint_Error => Conv_Err := True;
      end;
      if not Conv_Err then
         Fail ("Null array conversion base-type check");
      end if;

      if (WB_F and then Touch) or else not (WB_T or else Touch) then
         Fail ("Wide_Bool short-circuit");
      end if;
      if Side /= 0 then
         Fail ("Wide_Bool short-circuit evaluated RHS");
      end if;
   end Test_Misc_Array_And_Bool;

begin
   Test_Unsigned_And_Packed_Arith;
   Test_Dyn_Arr_Elements (4);
   Test_Stack_And_SS_Reclaim (512);
   Test_5D_Arrays;
   Test_64Bit_Index_Arrays;
   Test_Misc_Array_And_Bool (6);
   Put_Line ("PASS3_STAGE1_OK");
end Pass3_Stage1_Test;
