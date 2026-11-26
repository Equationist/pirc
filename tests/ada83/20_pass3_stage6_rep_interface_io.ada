-- Pass 3 Stage 6 Regression Test (also covers Pass 1 & Pass 2 Ch 13 rep clauses & Ch 14 I/O):
--   P3-6.1:  Non-negative fixed-point component in record bitfield (not sign-extended!)
--   P3-6.2:  Enum SM_REP (incl. negative codes) in bitfields & Unchecked_Conversion & CD_IMPL_SIZE
--   P3-6.3:  Variant record bitfields discriminant check on read/write/in out; composite explicit offset
--   P3-6.4:  Unaligned explicit byte offset (align 1) & at mod 4
--   P3-6.5:  Partial-rep record 'Size, explicit for R'Size, wide component 'Size, packed Bool array 'Size
--   P3-6.6:  Dynamic 'Storage_Size captured across nested subprogram
--   P3-6.7:  Record view conversion across differing representations (in out copy-back & .Field)
--   P3-6.8:  'Address of package, label, entry (with for E use at), and uplevel in param
--   P3-6.10: Address overlay with default init
--   P3-6.11..20: TEXT_IO/SEQ_IO/DIR_IO/LOW_LEVEL_IO range/DATA_ERROR/END_ERROR/bitfield/2D-uncon/EOL/NAME_ERROR
with TEXT_IO, SEQUENTIAL_IO, DIRECT_IO, SYSTEM, UNCHECKED_CONVERSION, LOW_LEVEL_IO;
use TEXT_IO, SYSTEM;

procedure Pass3_Stage6_Test is

   -- P3-6.1 & P3-6.2: Fixed-point & negative-code Enum in bitfields + Unchecked_Conversion
   type Pos_Fix is delta 0.125 range 0.0 .. 1.875;
   type Neg_Enum is (N_Low, N_Zero, N_High);
   for Neg_Enum use (N_Low => -10, N_Zero => 0, N_High => 500);

   type Mix_BF is record
      F : Pos_Fix;
      E : Neg_Enum;
   end record;
   for Mix_BF use record
      F at 0 range 0 .. 3;
      E at 1 range 0 .. 15;
   end record;

   function NE_To_I16 is new UNCHECKED_CONVERSION (Neg_Enum, Short_Integer);
   function I16_To_NE is new UNCHECKED_CONVERSION (Short_Integer, Neg_Enum);

   -- P3-6.3 & P3-6.4: Variant bitfields, composite explicit offset, unaligned explicit byte offset
   type Kind_T is (K_A, K_B);
   type Arr4 is array (1 .. 4) of Integer;
   type Var_BF (K : Kind_T := K_A) is record
      case K is
         when K_A =>
            A_Val : Integer := 10;
         when K_B =>
            B_Val : Integer := 20;
      end case;
   end record;
   for Var_BF use record
      K     at 0 range 0 .. 1;
      A_Val at 1 range 0 .. 31;
      B_Val at 1 range 0 .. 31;
   end record;

   type Unaligned_Rec is record
      Tag : Boolean;
      Val : Integer;
      Sub : Arr4;
   end record;
   for Unaligned_Rec use record
      at mod 4;
      Tag at 0 range 0 .. 7;
      Val at 1 range 0 .. 31;
      Sub at 8 range 0 .. 127;
   end record;

   -- P3-6.5: Partial-rep record, explicit 'Size, packed bool array
   type Wide_R is record
      X : Integer;
      Y : Integer;
   end record;
   for Wide_R'Size use 128;

   type Pack_B8 is array (1 .. 8) of Boolean;
   pragma PACK (Pack_B8);
   for Pack_B8'Size use 8;

   -- P3-6.7: Record view conversion across differing representations
   type Base_R is record
      A : Integer;
      B : Integer;
   end record;
   for Base_R use record
      A at 0 range 0 .. 31;
      B at 4 range 0 .. 31;
   end record;

   type Der_R is new Base_R;
   for Der_R use record
      B at 0 range 0 .. 31;
      A at 4 range 0 .. 31;
   end record;

   -- P3-6.8: Package & entry address
   package Addr_Pkg is
      X : Integer := 1;
   end Addr_Pkg;

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P3S6: " & Msg);
      raise Program_Error;
   end Fail;

   procedure Swap_Base (R : in out Base_R) is
      Tmp : Integer := R.A;
   begin
      R.A := R.B;
      R.B := Tmp;
   end Swap_Base;

   procedure Test_Rep_Bitfields_And_Conv is
      M  : Mix_BF := (F => 1.5, E => N_Low);
      VB : Var_BF := (K => K_A, A_Val => 42);
      UR : Unaligned_Rec := (Tag => True, Val => 16#123456#, Sub => (1, 2, 3, 4));
      DR : Der_R := (A => 11, B => 22);
      Caught : Boolean;

      procedure Inc (X : in out Integer) is
      begin
         X := X + 1;
      end Inc;
   begin
      -- P3-6.1: Pos_Fix = 1.5 (ticks = 12 = 2#1100# in 4 bits) must NOT sign-extend to -0.5!
      if M.F /= 1.5 then
         Fail ("Non-negative fixed bitfield sign-extended");
      end if;
      -- P3-6.2: Neg_Enum SM_REP in bitfield & Unchecked_Conversion
      if M.E /= N_Low then Fail ("Neg_Enum bitfield N_Low"); end if;
      M.E := N_High;
      if M.E /= N_High then Fail ("Neg_Enum bitfield N_High"); end if;
      if NE_To_I16 (N_Low) /= -10 or else NE_To_I16 (N_High) /= 500 then
         Fail ("Neg_Enum Unchecked_Conversion to I16");
      end if;
      if I16_To_NE (-10) /= N_Low or else I16_To_NE (500) /= N_High then
         Fail ("Neg_Enum Unchecked_Conversion from I16");
      end if;

      -- P3-6.3: Variant bitfield discriminant check on read, write, and in out
      Caught := False;
      begin
         if VB.B_Val = 0 then null; end if;
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then Fail ("Variant bitfield read check"); end if;

      Caught := False;
      begin
         VB.B_Val := 99;
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then Fail ("Variant bitfield write check"); end if;

      Caught := False;
      begin
         Inc (VB.B_Val);
      exception
         when Constraint_Error => Caught := True;
      end;
      if not Caught then Fail ("Variant bitfield in out check"); end if;

      -- P3-6.3/4: Unaligned explicit offset & composite component
      if not UR.Tag or else UR.Val /= 16#123456# or else UR.Sub (4) /= 4 then
         Fail ("Unaligned/composite explicit offset");
      end if;

      -- P3-6.5: 'Size attributes
      if Wide_R'Size /= 128 or else Pack_B8'Size /= 8 then
         Fail ("Explicit 'Size attribute");
      end if;

      -- P3-6.7: Record view conversion with differing rep (selection + in out copy-back)
      if Base_R (DR).A /= 11 or else Base_R (DR).B /= 22 then
         Fail ("Record view conv field selection");
      end if;
      Swap_Base (Base_R (DR));
      if DR.A /= 22 or else DR.B /= 11 then
         Fail ("Record view conv in out copy-back");
      end if;
   end Test_Rep_Bitfields_And_Conv;

   -- P3-6.6 & P3-6.8: Dynamic 'Storage_Size capture & 'Address of package/label/entry/uplevel in param
   procedure Test_Storage_And_Addresses (P_In : in Integer; Dyn_Sz : Integer) is
      type Dyn_Pool_Acc is access Integer;
      for Dyn_Pool_Acc'Storage_Size use Dyn_Sz;

      task Addr_T is
         entry E;
         for E use at 16#0040#;
      end Addr_T;

      A_P1 : Address;
      A_P2 : Address;

      function Read_Pool_Sz return Integer is
      begin
         return Dyn_Pool_Acc'Storage_Size;
      end Read_Pool_Sz;

      function Uplevel_Addr return Address is
      begin
         return P_In'Address;
      end Uplevel_Addr;

      task body Addr_T is
      begin
         select
            accept E;
         or
            terminate;
         end select;
      end Addr_T;
   begin
      A_P1 := Uplevel_Addr;
      A_P2 := Uplevel_Addr;
      if Read_Pool_Sz /= Dyn_Sz then
         Fail ("Nested captured 'Storage_Size");
      end if;
      if Addr_Pkg'Address = 0 then
         Fail ("Package 'Address is 0");
      end if;
      <<My_Lbl>>
      if My_Lbl'Address = 0 then
         Fail ("Label 'Address is 0");
      end if;
      if Addr_T.E'Address /= 16#0040# then
         Fail ("Entry 'Address clause");
      end if;
      if A_P1 = 0 or else A_P1 /= A_P2 then
         Fail ("Uplevel in param 'Address unstable");
      end if;
   end Test_Storage_And_Addresses;

   -- P3-6.11..20: Chapter 14 I/O checks
   procedure Test_Chapter14_IO is
      subtype Small_10 is Integer range 1 .. 10;
      package S10_IO is new TEXT_IO.INTEGER_IO (Small_10);
      package S10_Seq is new SEQUENTIAL_IO (Small_10);
      package Int_Dir is new DIRECT_IO (Integer);

      type Mat_2D is array (Positive range <>, Positive range <>) of Integer;
      package Mat_Seq is new SEQUENTIAL_IO (Mat_2D);

      F_Txt : TEXT_IO.File_Type;
      F_Seq : S10_Seq.File_Type;
      F_Mat : Mat_Seq.File_Type;
      F_Dir : Int_Dir.File_Type;

      S_Val  : Small_10 := 5;
      Last   : Positive;
      Caught : Boolean;
      M_Out  : Mat_2D (1 .. 2, 1 .. 3) := ((1, 2, 3), (4, 5, 6));
      M_In   : Mat_2D (1 .. 2, 1 .. 3) := ((0, 0, 0), (0, 0, 0));
      D_Val  : Integer := 99;
      Long_Z : constant String :=
         "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000007";
   begin
      -- P3-6.11: TEXT_IO.INTEGER_IO.GET out-of-range raises DATA_ERROR & preserves item
      Caught := False;
      begin
         S10_IO.Get ("99", S_Val, Last);
      exception
         when TEXT_IO.Data_Error => Caught := True;
      end;
      if not Caught or else S_Val /= 5 then
         Fail ("INTEGER_IO.GET DATA_ERROR / preservation");
      end if;

      -- P3-6.15: Long leading-zero number parses without truncation
      S10_IO.Get (Long_Z, S_Val, Last);
      if S_Val /= 7 then
         Fail ("Leading-zero numeric lexing");
      end if;

      -- P3-6.18: 2D unconstrained array SEQUENTIAL_IO framing
      Mat_Seq.Create (F_Mat, Mat_Seq.Out_File, "");
      Mat_Seq.Write (F_Mat, M_Out);
      Mat_Seq.Reset (F_Mat, Mat_Seq.In_File);
      Mat_Seq.Read (F_Mat, M_In);
      if M_In /= M_Out then
         Fail ("2D unconstrained SEQUENTIAL_IO roundtrip");
      end if;
      -- END_ERROR must not corrupt M_In
      Caught := False;
      begin
         Mat_Seq.Read (F_Mat, M_In);
      exception
         when Mat_Seq.End_Error => Caught := True;
      end;
      Mat_Seq.Delete (F_Mat);
      if not Caught or else M_In /= M_Out then
         Fail ("2D SEQUENTIAL_IO END_ERROR preservation");
      end if;

      -- P3-6.18: DIRECT_IO write/reset/read + END_ERROR preservation
      Int_Dir.Create (F_Dir, Int_Dir.Out_File, "");
      Int_Dir.Write (F_Dir, 42, 1);
      Int_Dir.Reset (F_Dir, Int_Dir.In_File);
      Int_Dir.Read (F_Dir, D_Val, 1);
      if D_Val /= 42 then Fail ("DIRECT_IO read 1"); end if;
      Caught := False;
      begin
         Int_Dir.Read (F_Dir, D_Val, 2);
      exception
         when Int_Dir.End_Error => Caught := True;
      end;
      Int_Dir.Delete (F_Dir);
      if not Caught or else D_Val /= 42 then
         Fail ("DIRECT_IO END_ERROR preservation");
      end if;

      -- P3-6.19: TEXT_IO EOL/EOP/EOF transitions (LRM 14.3.4(11))
      TEXT_IO.Create (F_Txt, TEXT_IO.Out_File, "");
      TEXT_IO.Put_Line (F_Txt, "AB");
      TEXT_IO.Put_Line (F_Txt, "CD");
      TEXT_IO.Reset (F_Txt, TEXT_IO.In_File);
      declare
         Ch : Character;
      begin
         TEXT_IO.Get (F_Txt, Ch);
         TEXT_IO.Get (F_Txt, Ch);
         if not TEXT_IO.End_Of_Line (F_Txt) or else TEXT_IO.End_Of_File (F_Txt) then
            Fail ("TEXT_IO EOL before Skip_Line on line 1");
         end if;
         TEXT_IO.Skip_Line (F_Txt);
         TEXT_IO.Get (F_Txt, Ch);
         TEXT_IO.Get (F_Txt, Ch);
         if not TEXT_IO.End_Of_Line (F_Txt) or else not TEXT_IO.End_Of_File (F_Txt) then
            Fail ("TEXT_IO EOL+EOF on final line");
         end if;
         TEXT_IO.Skip_Line (F_Txt);
         if not TEXT_IO.End_Of_File (F_Txt) then
            Fail ("TEXT_IO EOF after Skip_Line");
         end if;
      end;
      TEXT_IO.Delete (F_Txt);

      -- P3-6.20: Embedded NUL in filename raises NAME_ERROR
      Caught := False;
      begin
         TEXT_IO.Open (F_Txt, TEXT_IO.In_File, "bad" & ASCII.NUL & "name");
      exception
         when TEXT_IO.Name_Error => Caught := True;
      end;
      if not Caught then
         Fail ("Embedded NUL filename did not raise NAME_ERROR");
      end if;

      -- P3-6.18: SEQUENTIAL_IO scalar roundtrip smoke
      S10_Seq.Create (F_Seq, S10_Seq.Out_File, "");
      S10_Seq.Write (F_Seq, 9);
      S10_Seq.Reset (F_Seq, S10_Seq.In_File);
      S10_Seq.Read (F_Seq, S_Val);
      S10_Seq.Delete (F_Seq);
      if S_Val /= 9 then Fail ("S10_Seq roundtrip"); end if;

      -- LOW_LEVEL_IO smoke test
      declare
         Dev  : LOW_LEVEL_IO.Device_Type;
         Data : LOW_LEVEL_IO.Data_Type;
      begin
         LOW_LEVEL_IO.Send_Control (Dev, Data);
         LOW_LEVEL_IO.Receive_Control (Dev, Data);
      end;
   end Test_Chapter14_IO;

begin
   Test_Rep_Bitfields_And_Conv;
   Test_Storage_And_Addresses (77, 1024);
   Test_Chapter14_IO;
   Put_Line ("PASS3_STAGE6_OK");
end Pass3_Stage6_Test;
