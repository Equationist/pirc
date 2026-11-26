-- Pass 4 Regression Test 1 (Stages P4-1 & P4-3):
--   - FLOAT / LONG_FLOAT range & static eval (0 * 10**999, 1 ** 10**10, Integer'First)
--   - Abort of TS_CREATED task before activation (no TASKING_ERROR, LRM 9.10(6))
--   - INTEGER_IO.GET invalid base consumes full based literal before DATA_ERROR (LRM 14.3.7(7))
--   - Rep-claused record alignment > 1 embedded inside outer record (PIR align <P2>)
--   - Generic Ctx.Types isolation across distinct instantiations + generic-local exception isolation
with TEXT_IO;
use TEXT_IO;

procedure Pass4_Rt_Core_Test is
   procedure Fail (Msg : String);

   -- P4-6 / P4-3 static universal arithmetic edge cases
   Zero_Huge : constant := 0 * (10 ** 300);
   One_Huge  : constant := 1 ** (10 ** 10);
   Neg_One_O : constant := (-1) ** 1000000001;
   Neg_One_E : constant := (-1) ** 1000000000;
   Min_I32   : constant Integer := -2_147_483_648;

   -- P4-3: Rep-claused record with align > 1 embedded inside outer record
   type Aligned_Rec is record
      A : Integer;
      B : Integer;
   end record;
   for Aligned_Rec use record
      at mod 8;
      A at 0 range 0 .. 31;
      B at 4 range 0 .. 31;
   end record;

   type Outer_Rec is record
      Tag : Character;
      Sub : Aligned_Rec;
      End_Mark : Integer;
   end record;

   -- P4-3: Generic Ctx.Types & generic-local exception isolation
   generic
      type Elem is private;
      Default_V : Elem;
   package Gen_Box is
      Local_Exc : exception;
      type Box_Rec is record
         Val : Elem := Default_V;
         Tag : Integer := 77;
      end record;
      procedure Raise_Local;
      function Make_Box return Box_Rec;
   end Gen_Box;

   O  : Outer_Rec;
   Big_F : Float := 1.0E35;
   Big_LF : Long_Float := 1.0E200;
   Caught_Right : Boolean := False;

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P4_1: " & Msg);
      raise Program_Error;
   end Fail;

   package body Gen_Box is
      procedure Raise_Local is
      begin
         raise Local_Exc;
      end Raise_Local;

      function Make_Box return Box_Rec is
         B : Box_Rec;
      begin
         return B;
      end Make_Box;
   end Gen_Box;

   package Int_Box is new Gen_Box (Integer, 12345);
   package Flt_Box is new Gen_Box (Float, 3.5);

   -- P4-1: INTEGER_IO.GET invalid base consumes through closing '#'
   procedure Test_IO_Bad_Base is
      package Int_IO is new TEXT_IO.INTEGER_IO (Integer);
      F   : File_Type;
      V   : Integer := 0;
      Ch  : Character := '?';
      Got_DE : Boolean := False;
   begin
      Create (F, Out_File, "");
      Put_Line (F, "17#1A#Z");
      Reset (F, In_File);
      begin
         Int_IO.Get (F, V);
      exception
         when DATA_ERROR =>
            Got_DE := True;
      end;
      if not Got_DE then
         Delete (F);
         Fail ("Expected DATA_ERROR for base 17");
      end if;
      Get (F, Ch);
      Delete (F);
      if Ch /= 'Z' then
         Fail ("INTEGER_IO.GET stopped at wrong char: " & Ch);
      end if;
   end Test_IO_Bad_Base;

   -- P4-1: Abort task before activation (TS_CREATED) completes without TASKING_ERROR
   procedure Test_Abort_Before_Act is
      task type T_Type;
      type T_Acc is access T_Type;
      Got_TE : Boolean := False;

      function Make_And_Abort (P : T_Acc) return Integer is
      begin
         abort P.all;
         return 1;
      end Make_And_Abort;

      task body T_Type is
      begin
         null;
      end T_Type;
   begin
      begin
         declare
            P : T_Acc := new T_Type;
            X : Integer := Make_And_Abort (P);
         begin
            if X /= 1 then Fail ("Make_And_Abort"); end if;
            while not P'Terminated loop
               delay 0.01;
            end loop;
         end;
      exception
         when TASKING_ERROR =>
            Got_TE := True;
      end;
      if Got_TE then
         Fail ("Unexpected TASKING_ERROR on abort");
      end if;
   end Test_Abort_Before_Act;
begin
   if Zero_Huge /= 0 or else One_Huge /= 1 or else Neg_One_O /= -1 or else Neg_One_E /= 1 then
      Fail ("Static universal arithmetic");
   end if;
   if Min_I32 /= Integer'First or else Min_I32 + 1 /= -2_147_483_647 then
      Fail ("Min_I32");
   end if;
   if Big_F < 1.0E34 or else Big_LF < 1.0E199 then
      Fail ("Float/Long_Float range");
   end if;

   O := (Tag => 'K', Sub => (A => 111, B => 222), End_Mark => 333);
   if O.Tag /= 'K' or else O.Sub.A /= 111 or else O.Sub.B /= 222 or else O.End_Mark /= 333 then
      Fail ("Aligned embedded rep record");
   end if;

   declare
      IB : Int_Box.Box_Rec := Int_Box.Make_Box;
      FB : Flt_Box.Box_Rec := Flt_Box.Make_Box;
   begin
      if IB.Val /= 12345 or else IB.Tag /= 77 then
         Fail ("Int_Box generic types");
      end if;
      if FB.Val < 3.4 or else FB.Val > 3.6 or else FB.Tag /= 77 then
         Fail ("Flt_Box generic types");
      end if;
   end;

   begin
      Int_Box.Raise_Local;
   exception
      when Flt_Box.Local_Exc =>
         Fail ("Generic-local exception collided across instances!");
      when Int_Box.Local_Exc =>
         Caught_Right := True;
   end;
   if not Caught_Right then
      Fail ("Generic-local exception not caught");
   end if;

   Test_IO_Bad_Base;
   Test_Abort_Before_Act;

   Put_Line ("PASS4_RT_CORE_OK");
end Pass4_Rt_Core_Test;
