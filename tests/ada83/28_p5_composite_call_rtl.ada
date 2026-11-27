-- Pass-5 regression test: R3 (Scalar_Target/Bitfields), R4 (Composite Layout/Rep Conv/Deep Copy), R9 (RTL I/O)
with Text_IO; use Text_IO;
with Sequential_IO;
with Calendar;
with Unchecked_Deallocation;

procedure P5_Composite_Call_RTL is
   Failed : Integer := 0;
   procedure Check (Cond : Boolean; Label : String);

   -- R3: Bitfield & custom-SM_REP enum record + package renaming + view conversions
   type Rep_Enum is (R_Low, R_Mid, R_High);
   for Rep_Enum use (R_Low => 10, R_Mid => 20, R_High => 30);
   for Rep_Enum'Size use 8;

   subtype Small_Int is Integer range 0 .. 100;
   type Int_Ptr is access Integer;
   procedure Free_Int is new Unchecked_Deallocation (Integer, Int_Ptr);

   type Packed_Rec is record
      A : Small_Int := 0;
      E : Rep_Enum := R_Low;
      B : Small_Int := 0;
      Y : Calendar.Year_Number := 2000;
      M : Calendar.Month_Number := 1;
      D : Calendar.Day_Number := 1;
      S : Calendar.Day_Duration := 0.0;
   end record;
   for Packed_Rec use record
      A at 0 range 0 .. 6;
      E at 1 range 0 .. 7;
      B at 2 range 1 .. 7;
      Y at 4 range 0 .. 15;
      M at 6 range 0 .. 7;
      D at 7 range 0 .. 7;
      S at 8 range 0 .. 63;
   end record;

   package BF_Pkg is
      R : Packed_Rec;
      BF_A : Small_Int renames R.A;
      BF_E : Rep_Enum renames R.E;
      Shared_P : Int_Ptr := null;
      pragma Shared (Shared_P);
   end BF_Pkg;

   -- R4: Unaligned composite record rep clause + derived rep-changing record
   type Inner_R is record
      X : Integer := 11;
      Y : Integer := 22;
   end record;

   type Unaligned_R is record
      Tag : Character := 'Z';
      Val : Inner_R;
   end record;
   for Unaligned_R use record
      Tag at 0 range 0 .. 7;
      Val at 1 range 0 .. 63;
   end record;

   type Base_Disc (D : Integer := 1) is record
      A : Integer := 0;
      B : Integer := 0;
   end record;
   for Base_Disc use record
      D at 0 range 0 .. 31;
      A at 4 range 0 .. 31;
      B at 8 range 0 .. 31;
   end record;

   type Swapped_Disc is new Base_Disc;
   for Swapped_Disc use record
      D at 0 range 0 .. 31;
      B at 4 range 0 .. 31;
      A at 8 range 0 .. 31;
   end record;

   -- R4: Record containing dynamic-bound array component default-initialized
   Dyn_N : Integer := 3;
   type Dyn_Arr is array (1 .. Dyn_N) of Integer;
   type Dyn_Holder is record
      K : Integer := 99;
      A : Dyn_Arr;
   end record;

   -- R4: Subtype allocator failing constraint must NOT charge pool storage
   type Pool_Arr is array (Positive range <>) of Integer;
   type Pool_Acc is access Pool_Arr;
   for Pool_Acc'Storage_Size use 256;

   -- R6 (P5-1 #2): Derived enum overloads keep distinct PIR tags
   type E_Root is (K1, K2);
   type E_Der is new E_Root;

   -- R9: Float_IO for Float (f32), Integer_IO with DN_RANGE_ATTRIBUTE, Sequential_IO DATA_ERROR vs CONSTRAINT_ERROR
   package SF_IO is new Text_IO.Float_IO (Float);
   type Idx_Arr is array (10 .. 20) of Integer;
   subtype Rng_Sub is Integer range Idx_Arr'Range;
   package Rng_IO is new Text_IO.Integer_IO (Rng_Sub);
   package Enum_IO is new Text_IO.Enumeration_IO (Rep_Enum);

   subtype Base_D2 is Base_Disc (D => 2);
   package Seq_Base is new Sequential_IO (Base_Disc);
   package Seq_D2 is new Sequential_IO (Base_D2);

   -- Entry OUT/IN OUT with bitfield
   task BF_Worker is
      entry Update (V : in out Integer; E : out Rep_Enum);
   end BF_Worker;

   U1, U2 : Unaligned_R;
   SD : Swapped_Disc := (D => 2, A => 10, B => 20);
   DH : Dyn_Holder;
   SF_Val : Float := 0.0;
   Last_I : Positive;
   Caught_Data : Boolean;
   Caught_CE   : Boolean;

   procedure Check (Cond : Boolean; Label : String) is
   begin
      if not Cond then
         Put_Line ("FAIL: " & Label);
         Failed := Failed + 1;
      end if;
   end Check;

   procedure Bump_Small (X : in out Integer) is
   begin
      X := X + 7;
   end Bump_Small;

   procedure Set_Enum (X : out Rep_Enum) is
   begin
      X := R_High;
   end Set_Enum;

   procedure Swap_In_Out (X : in out Base_Disc) is
      T : constant Integer := X.A;
   begin
      X.A := X.B + 1;
      X.B := T + 2;
   end Swap_In_Out;

   function Make_Swapped (A, B : Integer) return Swapped_Disc is
      R : Swapped_Disc := (D => 3, A => A, B => B);
   begin
      return R;
   end Make_Swapped;

   function Tag_Of (X : E_Root) return Integer is
   begin
      if X = K1 then return 101; else return 102; end if;
   end Tag_Of;

   function Tag_Of (X : E_Der) return Integer is
   begin
      if X = K1 then return 201; else return 202; end if;
   end Tag_Of;

   task body BF_Worker is
   begin
      accept Update (V : in out Integer; E : out Rep_Enum) do
         V := V + 10;
         E := R_Mid;
      end Update;
   end BF_Worker;
begin
   -- 1. Bitfield & renamed bitfield view conversions + entry OUT/IN OUT + Calendar.Split + Shared Unchecked_Deallocation
   BF_Pkg.BF_A := 12;
   Bump_Small (Integer (BF_Pkg.BF_A));
   Check (BF_Pkg.R.A = 19, "bf_ren_view_conv");
   Set_Enum (BF_Pkg.BF_E);
   Check (BF_Pkg.R.E = R_High, "bf_enum_out");

   BF_Worker.Update (Integer (BF_Pkg.R.B), BF_Pkg.R.E);
   Check (BF_Pkg.R.B = 10 and then BF_Pkg.R.E = R_Mid, "bf_entry_out");

   Calendar.Split (Calendar.Time_Of (2025, 6, 15, 3600.0), BF_Pkg.R.Y, BF_Pkg.R.M, BF_Pkg.R.D, BF_Pkg.R.S);
   Check (BF_Pkg.R.Y = 2025 and then BF_Pkg.R.M = 6 and then BF_Pkg.R.D = 15, "calendar_split_bf");

   BF_Pkg.Shared_P := new Integer'(42);
   Check (BF_Pkg.Shared_P.all = 42, "shared_alloc");
   Free_Int (BF_Pkg.Shared_P);
   Check (BF_Pkg.Shared_P = null, "shared_free");

   -- 2. Unaligned record equality & copy
   U1 := (Tag => 'A', Val => (X => 123, Y => 456));
   U2 := U1;
   Check (U1 = U2 and then U2.Val.X = 123 and then U2.Val.Y = 456, "unaligned_eq_copy");

   -- 3. Rep-changing derived record in subprogram call & function return
   Swap_In_Out (Base_Disc (SD));
   Check (SD.A = 21 and then SD.B = 12, "rep_conv_inout");
   SD := Make_Swapped (33, 44);
   Check (SD.D = 3 and then SD.A = 33 and then SD.B = 44, "rep_conv_ret");

   -- 4. Dynamic array component default-init
   Check (DH.K = 99 and then DH.A'First = 1 and then DH.A'Last = 3, "dyn_holder_init");

   -- 5. Subtype allocator failing constraint does not leak pool quota
   declare
      P : Pool_Acc;
      Bad_Hi : Integer := 0;
   begin
      for I in 1 .. 50 loop
         begin
            P := new Pool_Arr (1 .. Bad_Hi);
            -- 1..0 is null array, legal; test failing subtype range via negative index below
            if P /= null then
               null;
            end if;
         exception
            when Constraint_Error => null;
         end;
      end loop;
      Bad_Hi := -1;
      for I in 1 .. 50 loop
         begin
            P := new Pool_Arr (Bad_Hi .. 1); -- Positive lower bound -1 fails Constraint_Error BEFORE pool alloc!
            Check (False, "alloc_should_fail");
         exception
            when Constraint_Error => null;
         end;
      end loop;
      P := new Pool_Arr (1 .. 4);
      Check (P'Length = 4, "pool_not_exhausted");
   end;

   -- 6. Derived enum overloads
   Check (Tag_Of (E_Root'(K2)) = 102 and then Tag_Of (E_Der'(K2)) = 202, "derived_overload");

   -- 7. Float_IO.Get on Short_Float (f32) + Enumeration_IO.Get into bitfield + Integer_IO on DN_RANGE_ATTRIBUTE
   SF_IO.Get ("3.25", SF_Val, Last_I);
   Check (SF_Val > 3.2 and then SF_Val < 3.3 and then Last_I = 4, "sf_io_get_str");
   Enum_IO.Get ("R_HIGH", BF_Pkg.R.E, Last_I);
   Check (BF_Pkg.R.E = R_High, "enum_io_get_bf");

   declare
      RV : Rng_Sub := 15;
   begin
      Rng_IO.Get ("18", RV, Last_I);
      Check (RV = 18, "rng_attr_io_ok");
      Caught_Data := False;
      begin
         Rng_IO.Get ("25", RV, Last_I);
      exception
         when Text_IO.Data_Error => Caught_Data := True;
      end;
      Check (Caught_Data, "rng_attr_io_data_err");
   end;

   -- 8. Sequential_IO.Read rep-changing conversion + DATA_ERROR vs CONSTRAINT_ERROR on discriminants
   declare
      F_Write : Seq_Base.File_Type;
      F_Read2 : Seq_D2.File_Type;
      F_ReadB : Seq_Base.File_Type;
      Fname   : constant String := "/tmp/p5_seq_disc_test.dat";
      Rec_D1  : constant Base_Disc := (D => 1, A => 77, B => 88);
      Rec_D2  : constant Base_Disc := (D => 2, A => 55, B => 66);
      Got_D2  : Base_D2;
      Got_Sw  : Swapped_Disc (D => 2);
      Got_D1  : Base_Disc (D => 1);
   begin
      Seq_Base.Create (F_Write, Seq_Base.Out_File, Fname);
      Seq_Base.Write (F_Write, Rec_D1);
      Seq_Base.Write (F_Write, Rec_D2);
      Seq_Base.Close (F_Write);

      -- Reading D=1 record through Sequential_IO(Base_D2) must raise DATA_ERROR (not Constraint_Error!)
      Seq_D2.Open (F_Read2, Seq_D2.In_File, Fname);
      Caught_Data := False;
      begin
         Seq_D2.Read (F_Read2, Got_D2);
      exception
         when Seq_D2.Data_Error => Caught_Data := True;
      end;
      Check (Caught_Data, "seq_io_elem_disc_data_err");
      Seq_D2.Read (F_Read2, Got_D2);
      Check (Got_D2.D = 2 and then Got_D2.A = 55 and then Got_D2.B = 66, "seq_io_d2_ok");
      Seq_D2.Close (F_Read2);

      -- Reading D=2 record through Sequential_IO(Base_Disc) into D=1 target must raise CONSTRAINT_ERROR,
      -- and reading into rep-changing view conversion Swapped_Disc must copy-back fields correctly!
      Seq_Base.Open (F_ReadB, Seq_Base.In_File, Fname);
      Seq_Base.Read (F_ReadB, Got_D1);
      Check (Got_D1.A = 77 and then Got_D1.B = 88, "seq_io_d1_ok");
      Caught_CE := False;
      begin
         Seq_Base.Read (F_ReadB, Got_D1);
      exception
         when Constraint_Error => Caught_CE := True;
      end;
      Check (Caught_CE, "seq_io_item_disc_ce");
      Seq_Base.Close (F_ReadB);

      Seq_Base.Open (F_ReadB, Seq_Base.In_File, Fname);
      Seq_Base.Read (F_ReadB, Got_D1); -- skip D=1
      Seq_Base.Read (F_ReadB, Base_Disc (Got_Sw));
      Check (Got_Sw.D = 2 and then Got_Sw.A = 55 and then Got_Sw.B = 66, "seq_io_rep_conv_read");
      Seq_Base.Delete (F_ReadB);
   end;

   if Failed = 0 then
      Put_Line ("P5_COMPOSITE_CALL_RTL_OK");
   else
      Put_Line ("P5_COMPOSITE_CALL_RTL_FAILED");
   end if;
end P5_Composite_Call_RTL;
