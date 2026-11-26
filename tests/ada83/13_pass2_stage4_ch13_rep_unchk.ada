-- Pass 2 Stage 4 Regression Test: Ch. 13 Rep Clauses, Bitfields, UNCHECKED_* & Sequential_IO
with TEXT_IO, SYSTEM, UNCHECKED_CONVERSION, UNCHECKED_DEALLOCATION, SEQUENTIAL_IO;
use TEXT_IO, SYSTEM;

procedure Pass2_Stage4_Ch13 is

   -- 1 & 2. Record with bitfields including a 64-bit field spanning 9 bytes (bit 3..66)
   type Small_U4 is range 0 .. 15;
   type Big64 is range -2**62 .. 2**62 - 1;
   type Bit_Rec is record
      Lo3  : Small_U4;
      Span : Big64;
      Hi5  : Small_U4;
      Mid  : Small_U4;
   end record;
   for Bit_Rec use record
      Lo3  at 0 range 0 .. 2;
      Span at 0 range 3 .. 66;
      Hi5  at 8 range 3 .. 7;
      Mid  at 0 range 72 .. 75;
   end record;

   -- 3. Variant record with DN_RECORD_REP (bitfield discriminant + float bitfield + variants)
   type Tag_E is (K_Int, K_Flt);
   type Rep_Var (K : Tag_E := K_Int) is record
      Guard : Small_U4 := 5;
      case K is
         when K_Int =>
            IV : Integer := 0;
         when K_Flt =>
            FV : Float := 0.0;
      end case;
   end record;
   for Rep_Var use record
      K     at 0 range 0 .. 1;
      Guard at 0 range 2 .. 5;
      IV    at 1 range 0 .. 31;
      FV    at 1 range 0 .. 31;
   end record;
   subtype Rep_Var_Int is Rep_Var (K_Int);
   subtype Rep_Var_Flt is Rep_Var (K_Flt);

   -- Derived variant record with different layout (for LRM 13.6 change of representation)
   type Der_Var is new Rep_Var;
   for Der_Var use record
      Guard at 0 range 0 .. 3;
      K     at 0 range 4 .. 5;
      IV    at 4 range 0 .. 31;
      FV    at 4 range 0 .. 31;
   end record;

   -- 4. UNCHECKED_CONVERSION types
   type Bool_Arr4 is array (1 .. 4) of Boolean;
   type Bool_Arr4_B is array (1 .. 4) of Boolean;
   function B2B is new UNCHECKED_CONVERSION (Bool_Arr4, Bool_Arr4_B);

   type Int_Ptr is access Integer;
   type Der_Ptr is new Int_Ptr;
   procedure Free_Int is new UNCHECKED_DEALLOCATION (Integer, Int_Ptr);

   type Long_F is digits 15;
   function Ptr_To_F64 is new UNCHECKED_CONVERSION (Int_Ptr, Long_F);
   function F64_To_Ptr is new UNCHECKED_CONVERSION (Long_F, Int_Ptr);
   function Ptr_To_I16 is new UNCHECKED_CONVERSION (Int_Ptr, Short_Integer);

   -- 130-literal enum with custom rep clause (tests zext indexing into @ada_erep_*)
   type Big_Enum is (
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
      E120, E121, E122, E123, E124, E125, E126, E127, E128, E129);
   for Big_Enum use (
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
      10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
      20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
      30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
      40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
      50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
      60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
      70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
      80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
      90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
      100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
      110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
      120, 121, 122, 123, 124, 125, 126, 127, 500, 999);
   function BE_To_Int is new UNCHECKED_CONVERSION (Big_Enum, Integer);

   -- 6. Scalar constant & deferred address clause with Init_Exp
   My_Const : constant Integer := 42;
   Target_Storage : Integer := 0;
   Overlay_Var : Integer := 12345;
   for Overlay_Var use at Target_Storage'Address;

   -- 7. Sequential_IO for scalar END_ERROR preservation check
   package Int_Seq_IO is new SEQUENTIAL_IO (Integer);

   -- Helper procedures for bitfield OUT / IN OUT tests
   procedure Bump_U4 (X : in out Small_U4) is
   begin
      X := X + 2;
   end Bump_U4;

   procedure Set_Int (X : in out Integer) is
   begin
      X := X + 3;
   end Set_Int;

   procedure Check_In_Param_Addr (P : in Integer) is
      A1 : Address := P'Address;
      A2 : Address := P'Address;
   begin
      if A1 /= A2 then
         raise Program_Error;
      end if;
   end Check_In_Param_Addr;

   procedure Test_Bitfields is
      R : Bit_Rec := (Lo3 => 5, Span => 16#1234_5678_9ABC_DEF#, Hi5 => 11, Mid => 9);
   begin
      -- Verify 9-byte spanning 64-bit bitfield + neighbors
      if R.Lo3 /= 5 or else R.Span /= 16#1234_5678_9ABC_DEF# or else R.Hi5 /= 11 or else R.Mid /= 9 then
         raise Program_Error;
      end if;
      -- Negative 64-bit spanning value
      R.Span := -16#1122_3344_5566_778#;
      if R.Lo3 /= 5 or else R.Span /= -16#1122_3344_5566_778# or else R.Hi5 /= 11 or else R.Mid /= 9 then
         raise Program_Error;
      end if;
      -- Bitfield IN OUT direct actual
      Bump_U4 (R.Lo3);
      if R.Lo3 /= 7 or else R.Span /= -16#1122_3344_5566_778# or else R.Hi5 /= 11 then
         raise Program_Error;
      end if;
      -- Bitfield IN OUT via scalar view conversion
      Set_Int (Integer (R.Hi5));
      if R.Hi5 /= 14 or else R.Lo3 /= 7 or else R.Span /= -16#1122_3344_5566_778# then
         raise Program_Error;
      end if;
      -- Obj.Bitfield'Size & normalized 'Position / 'First_Bit / 'Last_Bit
      if R.Lo3'Size /= 3 or else R.Mid'Size /= 4 then
         raise Program_Error;
      end if;
      if R.Mid'Position /= 9 or else R.Mid'First_Bit /= 0 or else R.Mid'Last_Bit /= 3 then
         raise Program_Error;
      end if;
   end Test_Bitfields;

   procedure Test_Variant_Rep is
      V1 : Rep_Var := (K => K_Flt, Guard => 7, FV => 3.5);
      V2 : Rep_Var := (K => K_Flt, Guard => 7, FV => 3.5);
      V3 : Rep_Var := (K => K_Flt, Guard => 7, FV => 4.5);
      DV : Der_Var;
      Back : Rep_Var;
      Bad_Caught : Boolean := False;
   begin
      if V1 /= V2 or else V1 = V3 then
         raise Program_Error;
      end if;
      if not (V1 in Rep_Var_Flt) or else (V1 in Rep_Var_Int) then
         raise Program_Error;
      end if;
      -- Change-of-representation conversion on variant record
      DV := Der_Var (V1);
      if DV.K /= K_Flt or else DV.Guard /= 7 or else DV.FV /= 3.5 then
         raise Program_Error;
      end if;
      Back := Rep_Var (DV);
      if Back /= V1 then
         raise Program_Error;
      end if;
      -- Discriminant check on DN_RECORD_REP constrained subtype
      begin
         declare
            X : Rep_Var_Int := V1;
         begin
            if X.IV = 0 then null; end if;
         end;
      exception
         when Constraint_Error => Bad_Caught := True;
      end;
      if not Bad_Caught then
         raise Program_Error;
      end if;
   end Test_Variant_Rep;

   procedure Test_Unchecked is
      BA1 : Bool_Arr4 := (True, False, True, True);
      BA2 : Bool_Arr4_B;
      P   : Int_Ptr := new Integer'(77);
      DP  : Der_Ptr := new Integer'(88);
      F   : Long_F;
      P2  : Int_Ptr;
      S16 : Short_Integer;
   begin
      BA2 := B2B (BA1);
      if not BA2 (1) or else BA2 (2) or else not BA2 (3) or else not BA2 (4) then
         raise Program_Error;
      end if;
      F  := Ptr_To_F64 (P);
      P2 := F64_To_Ptr (F);
      if P2 /= P or else P2.all /= 77 then
         raise Program_Error;
      end if;
      S16 := Ptr_To_I16 (P);
      if S16 = 0 and then P = null then
         raise Program_Error;
      end if;
      if BE_To_Int (E128) /= 500 or else BE_To_Int (E129) /= 999 then
         raise Program_Error;
      end if;
      Free_Int (P);
      Free_Int (Int_Ptr (DP));
      if P /= null or else DP /= null then
         raise Program_Error;
      end if;
   end Test_Unchecked;

   procedure Test_Addr_And_SeqIO is
      F   : Int_Seq_IO.File_Type;
      Val : Integer := 9876;
      Got_End : Boolean := False;
   begin
      if My_Const'Address /= My_Const'Address then
         raise Program_Error;
      end if;
      Check_In_Param_Addr (99);
      if Target_Storage /= 12345 or else Overlay_Var /= 12345 then
         raise Program_Error;
      end if;
      Int_Seq_IO.Create (F, Int_Seq_IO.Out_File, "");
      Int_Seq_IO.Write (F, 55);
      Int_Seq_IO.Reset (F, Int_Seq_IO.In_File);
      Int_Seq_IO.Read (F, Val);
      if Val /= 55 then
         raise Program_Error;
      end if;
      begin
         Int_Seq_IO.Read (F, Val);
      exception
         when Int_Seq_IO.End_Error => Got_End := True;
      end;
      Int_Seq_IO.Delete (F);
      -- END_ERROR must NOT have corrupted Val!
      if not Got_End or else Val /= 55 then
         raise Program_Error;
      end if;
   end Test_Addr_And_SeqIO;

begin
   Test_Bitfields;
   Test_Variant_Rep;
   Test_Unchecked;
   Test_Addr_And_SeqIO;
   Put_Line ("PASS2_STAGE4_OK");
end Pass2_Stage4_Ch13;
