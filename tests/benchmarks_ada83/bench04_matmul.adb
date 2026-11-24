with Text_IO;
with Calendar;
procedure Bench04_Matmul is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Flt_IO is new Text_IO.Float_IO(Long_Float);

   N     : constant Integer := 240;
   Iters : constant Integer := 7;
   type Matrix is array (Integer range 1 .. N, Integer range 1 .. N) of Long_Float;
   A, B, C : Matrix;

   T0, T1 : Calendar.Time;
   Ms : Long_Float;
   Best_Ms : Long_Float := 1.0E9;
   Sum : Long_Float := 0.0;

   procedure Init_Mats is
   begin
      for I in 1 .. N loop
         for J in 1 .. N loop
            A(I, J) := Long_Float((I * 3 + J * 7) mod 17) * 0.125;
            B(I, J) := Long_Float((I * 5 + J * 11) mod 19) * 0.0625;
            C(I, J) := 0.0;
         end loop;
      end loop;
   end Init_Mats;

   procedure Mul_Mats is
      Aik : Long_Float;
   begin
      for I in 1 .. N loop
         for J in 1 .. N loop
            C(I, J) := 0.0;
         end loop;
         for K in 1 .. N loop
            Aik := A(I, K);
            for J in 1 .. N loop
               C(I, J) := C(I, J) + Aik * B(K, J);
            end loop;
         end loop;
      end loop;
   end Mul_Mats;

begin
   Init_Mats;
   for It in 1 .. Iters loop
      T0 := Calendar.Clock;
      Mul_Mats;
      T1 := Calendar.Clock;
      Ms := Long_Float(Calendar."-"(T1, T0)) * 1000.0;
      if Ms < Best_Ms then
         Best_Ms := Ms;
      end if;
   end loop;
   for I in 1 .. N loop
      Sum := Sum + C(I, I);
   end loop;
   Int_IO.Put(Integer(Sum), Width => 0);
   Text_IO.Put(" ms=");
   Flt_IO.Put(Best_Ms, Fore => 1, Aft => 3, Exp => 0);
   Text_IO.New_Line;
end Bench04_Matmul;
