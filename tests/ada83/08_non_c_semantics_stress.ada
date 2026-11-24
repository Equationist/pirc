with Text_IO;
procedure Test08_Stress is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Long_IO is new Text_IO.Integer_IO(Long_Integer);

   Step_Err : exception;

   type Small_Enum is (E0, E1, E2);
   type Vec is array (Integer range <>) of Integer;

   S_Max : Short_Integer := Short_Integer'Last;
   S_Min : Short_Integer := Short_Integer'First;
   L_Max : Long_Integer  := Long_Integer'Last;
   L_Min : Long_Integer  := Long_Integer'First;
   E     : Small_Enum    := E2;
   Hit   : Integer := 0;
   Loop_Acc : Integer := 0;
   Mut_Val  : Integer := 0;

   procedure Mutate_And_Throw(X : in out Integer; Should_Throw : Boolean) is
   begin
      X := X + 17;
      if Should_Throw then
         raise Step_Err;
      end if;
   end Mutate_And_Throw;

   function Dyn_Sum(N : Integer) return Integer is
      A : Vec(1 .. N);
      S : Integer := 0;
   begin
      for I in 1 .. N loop
         A(I) := I * 3;
      end loop;
      for I in 1 .. N loop
         S := S + A(I);
      end loop;
      return S;
   end Dyn_Sum;

begin
   -- 1. 16-bit Short_Integer signed overflow (add, sub, mul)
   begin
      S_Max := S_Max + 1;
   exception
      when Constraint_Error => Hit := Hit + 1;
   end;
   begin
      S_Min := S_Min - 1;
   exception
      when Constraint_Error => Hit := Hit + 10;
   end;
   begin
      S_Max := 200;
      S_Max := S_Max * 200;
   exception
      when Constraint_Error => Hit := Hit + 100;
   end;

   -- 2. 64-bit Long_Integer signed overflow (add, sub, mul)
   begin
      L_Max := L_Max + 1;
   exception
      when Constraint_Error => Hit := Hit + 1000;
   end;
   begin
      L_Min := L_Min - 1;
   exception
      when Constraint_Error => Hit := Hit + 10000;
   end;
   begin
      L_Max := 4_000_000_000;
      L_Max := L_Max * 4_000_000_000;
   exception
      when Constraint_Error => Hit := Hit + 100000;
   end;

   -- 3. 8-bit Enum 'Succ overflow
   begin
      E := Small_Enum'Succ(E);
   exception
      when Constraint_Error => Hit := Hit + 1000000;
   end;

   Text_IO.Put("ovf_mask="); Int_IO.Put(Hit, Width => 0); Text_IO.New_Line;

   -- 4. Invoke inside a loop + callee mutates in-out param before throwing!
   Mut_Val := 5;
   Loop_Acc := 0;
   begin
      for I in 1 .. 10 loop
         Loop_Acc := Loop_Acc + I;
         Mutate_And_Throw(Mut_Val, I = 4);
      end loop;
   exception
      when Step_Err =>
         -- Expect Loop_Acc = 1+2+3+4 = 10, Mut_Val = 5 + 4*17 = 73!
         Text_IO.Put("loop_exc="); Int_IO.Put(Loop_Acc, Width => 0);
         Text_IO.Put(","); Int_IO.Put(Mut_Val, Width => 0);
         Text_IO.New_Line;
   end;

   -- 5. Repeated dynamic stack allocation inside a loop (verifies stacksave/stackrestore reclaims stack!)
   Loop_Acc := 0;
   for K in 1 .. 50 loop
      Loop_Acc := Loop_Acc + Dyn_Sum(K);
   end loop;
   Text_IO.Put("dyn_loop="); Int_IO.Put(Loop_Acc, Width => 0); Text_IO.New_Line;
end Test08_Stress;
