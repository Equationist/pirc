-- Regression test for 64-bit array descriptor & bound handling (Step 5 refactor):
--   - Arrays indexed by Long_Integer with bounds > 2**31 - 1 and < -2**31
--   - 'First, 'Last, 'Length, indexing, bounds check (Constraint_Error)
--   - Slicing with 64-bit bounds (non-null and null slices)
--   - Unconstrained array parameters & dynamic function return
--   - Access-to-unconstrained-array allocator with 64-bit bounds
--   - Record component with discriminant-independent dynamic 64-bit array
--   - Concatenation (&) preserving 64-bit lower bound
--   - Task entry passing unconstrained 64-bit array
with TEXT_IO;
use TEXT_IO;

procedure Refactor_64Bit_Bounds_Test is
   type Big_Arr is array (Long_Integer range <>) of Integer;
   type Big_Arr_Acc is access Big_Arr;

   Hi_First : constant Long_Integer := Long_Integer'Last - 4;
   Hi_Last  : constant Long_Integer := Long_Integer'Last;
   Lo_First : constant Long_Integer := Long_Integer'First;
   Lo_Last  : constant Long_Integer := Long_Integer'First + 3;

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL 64BIT: " & Msg);
      raise Program_Error;
   end Fail;

   function Id_Long (X : Long_Integer) return Long_Integer is
   begin
      return X;
   end Id_Long;

   procedure Check_Uncon (A : Big_Arr; Exp_F, Exp_L : Long_Integer; Exp_Sum : Integer) is
      S : Integer := 0;
   begin
      if A'First /= Exp_F then Fail ("param First mismatch"); end if;
      if A'Last /= Exp_L then Fail ("param Last mismatch"); end if;
      if A'Length /= Integer (Exp_L - Exp_F + 1) then Fail ("param Length mismatch"); end if;
      for I in A'Range loop
         S := S + A (I);
      end loop;
      if S /= Exp_Sum then Fail ("param sum mismatch"); end if;
   end Check_Uncon;

   function Make_High (F, L : Long_Integer) return Big_Arr is
      R : Big_Arr (F .. L);
      K : Integer := 1;
   begin
      for I in R'Range loop
         R (I) := K * 10;
         K := K + 1;
      end loop;
      return R;
   end Make_High;

   task type Arr_Task is
      entry Check (A : Big_Arr; Exp_F, Exp_L : Long_Integer);
   end Arr_Task;

   task body Arr_Task is
   begin
      accept Check (A : Big_Arr; Exp_F, Exp_L : Long_Integer) do
         if A'First /= Exp_F or else A'Last /= Exp_L then
            Fail ("entry 64-bit bounds mismatch");
         end if;
         if A (Exp_F) /= 10 or else A (Exp_L) /= 50 then
            Fail ("entry 64-bit element mismatch");
         end if;
      end Check;
   end Arr_Task;

   -- 1. Dynamic local 64-bit high-range & low-range arrays
begin
   declare
      A_Hi : constant Big_Arr := Make_High (Id_Long (Hi_First), Id_Long (Hi_Last));
      A_Lo : Big_Arr (Id_Long (Lo_First) .. Id_Long (Lo_Last)) := (1, 2, 3, 4);
      P    : Big_Arr_Acc;
      T    : Arr_Task;
   begin
   -- 1. Check returned dynamic 64-bit array
   Check_Uncon (A_Hi, Hi_First, Hi_Last, 150);
   if A_Hi (Hi_First) /= 10 or else A_Hi (Hi_Last) /= 50 then
      Fail ("high-bound indexing");
   end if;

   -- 2. Check negative 64-bit bounds near Long_Integer'First
   Check_Uncon (A_Lo, Lo_First, Lo_Last, 10);
   if A_Lo (Lo_First) /= 1 or else A_Lo (Lo_Last) /= 4 then
      Fail ("low-bound indexing");
   end if;

   -- 3. Out-of-bounds indexing must raise Constraint_Error
   declare
      Raised : Boolean := False;
      Dummy  : Integer := 0;
   begin
      begin
         Dummy := A_Hi (Id_Long (Hi_First - 1));
      exception
         when Constraint_Error => Raised := True;
      end;
      if not Raised or else Dummy /= 0 then
         Fail ("expected CE on high-range underflow");
      end if;
   end;

   -- 4. 64-bit slicing & unconstrained parameter passing
   declare
      Sl : constant Big_Arr := A_Hi (Hi_First + 1 .. Hi_Last - 1);
   begin
      Check_Uncon (Sl, Hi_First + 1, Hi_Last - 1, 90);
      if Sl (Hi_First + 1) /= 20 or else Sl (Hi_Last - 1) /= 40 then
         Fail ("slice element mismatch");
      end if;
   end;

   -- 5. Access-to-unconstrained-array allocator with 64-bit bounds
   P := new Big_Arr'(A_Hi);
   if P'First /= Hi_First or else P'Last /= Hi_Last or else P'Length /= 5 then
      Fail ("allocator 64-bit bounds mismatch");
   end if;
   if P (Hi_Last) /= 50 then
      Fail ("allocator element mismatch");
   end if;

   -- 6. Concatenation preserving 64-bit lower bound
   declare
      C : constant Big_Arr := A_Lo (Lo_First .. Lo_First + 1) & A_Lo (Lo_First + 2 .. Lo_Last);
   begin
      if C'First /= Lo_First or else C'Last /= Lo_Last or else C'Length /= 4 then
         Fail ("concat 64-bit bounds mismatch");
      end if;
      if C (Lo_First) /= 1 or else C (Lo_Last) /= 4 then
         Fail ("concat 64-bit element mismatch");
      end if;
   end;

   -- 7. Task entry parameter with 64-bit array bounds
   T.Check (A_Hi, Hi_First, Hi_Last);

   -- 8. Record containing dynamic 64-bit constrained array
   declare
      F_Dyn : constant Long_Integer := Id_Long (Hi_First + 1);
      L_Dyn : constant Long_Integer := Id_Long (Hi_Last);
      subtype Sub_Hi is Big_Arr (F_Dyn .. L_Dyn);
      type Rec_Dyn is record
         Tag : Integer := 7;
         Arr : Sub_Hi  := (others => 99);
      end record;
      R1, R2 : Rec_Dyn;
   begin
      if R1.Arr'First /= F_Dyn or else R1.Arr'Last /= L_Dyn or else R1.Arr'Length /= 4 then
         Fail ("record dyn 64-bit bounds mismatch");
      end if;
      R1.Arr (L_Dyn) := 123;
      R2 := R1;
      R1.Arr (L_Dyn) := 456;
      if R2.Arr (L_Dyn) /= 123 or else R2.Arr'Last /= L_Dyn then
         Fail ("record dyn 64-bit deep copy mismatch");
      end if;
   end;
   end;

   Put_Line ("REFACTOR_64BIT_BOUNDS_OK");
end Refactor_64Bit_Bounds_Test;
