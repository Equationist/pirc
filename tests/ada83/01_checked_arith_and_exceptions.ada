with Text_IO;
procedure Test01_Exc is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   My_Err    : exception;
   Other_Err : exception;
   X      : Integer := 0;
   Caught : Integer := 0;
   State  : Integer := 10;

   procedure Put_Int(Label : String; V : Integer) is
   begin
      Text_IO.Put(Label);
      Int_IO.Put(V, Width => 0);
      Text_IO.New_Line;
   end Put_Int;

   function Add_Nums(A, B : Integer) return Integer is
   begin
      return A + B;
   end Add_Nums;

   function Mul_Nums(A, B : Integer) return Integer is
   begin
      return A * B;
   end Mul_Nums;

   function Div_Nums(A, B : Integer) return Integer is
   begin
      return A / B;
   end Div_Nums;

   procedure Raise_Deep(Depth : Integer) is
   begin
      if Depth <= 0 then
         raise My_Err;
      else
         Raise_Deep(Depth - 1);
      end if;
   end Raise_Deep;

   procedure Reraise_Test is
   begin
      begin
         raise Other_Err;
      exception
         when Other_Err =>
            raise;
      end;
   end Reraise_Test;
begin
   -- 1. Signed addition overflow -> Constraint_Error
   begin
      X := Add_Nums(Integer'Last, 1);
      Caught := 0;
   exception
      when Constraint_Error =>
         Caught := 1;
   end;
   Put_Int("add_ovf=", Caught);

   -- 2. Signed multiply overflow -> Constraint_Error
   begin
      X := Mul_Nums(100_000, 100_000);
      Caught := 0;
   exception
      when Constraint_Error =>
         Caught := 2;
   end;
   Put_Int("mul_ovf=", Caught);

   -- 3. Division by zero -> Constraint_Error
   begin
      X := Div_Nums(42, 0);
      Caught := 0;
   exception
      when Constraint_Error =>
         Caught := 3;
   end;
   Put_Int("div_zero=", Caught);

   -- 4. INT_MIN / -1 -> Constraint_Error
   begin
      X := Div_Nums(Integer'First, -1);
      Caught := 0;
   exception
      when Constraint_Error =>
         Caught := 4;
   end;
   Put_Int("div_min=", Caught);

   -- 5. Ada mod vs rem semantics with negative numbers
   Put_Int("mod1=", (-7) mod 3);
   Put_Int("rem1=", (-7) rem 3);
   Put_Int("mod2=", 7 mod (-3));
   Put_Int("rem2=", 7 rem (-3));
   Put_Int("mod3=", (-7) mod (-3));
   Put_Int("rem3=", (-7) rem (-3));

   -- 6. Deep unwinding across recursive frames + state modified before invoke
   State := 10;
   begin
      State := State + 5;
      Raise_Deep(5);
      State := 999;
   exception
      when My_Err =>
         State := State + 100;
   end;
   Put_Int("deep_state=", State);

   -- 7. Bare reraise + multi-choice handler
   Caught := 0;
   begin
      Reraise_Test;
   exception
      when My_Err | Other_Err =>
         Caught := 77;
   end;
   Put_Int("reraise=", Caught);
end Test01_Exc;
