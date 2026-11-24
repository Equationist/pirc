with Text_IO;
with Calendar;
procedure Bench06_Recursion is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Flt_IO is new Text_IO.Float_IO(Long_Float);

   Iters : constant Integer := 9;
   T0, T1 : Calendar.Time;
   Ms : Long_Float;
   Best_Ms : Long_Float := 1.0E9;
   Res : Integer := 0;

   function Fib(N : Integer) return Integer is
   begin
      if N <= 1 then
         return N;
      end if;
      return Fib(N - 1) + Fib(N - 2);
   end Fib;

   function Tak(X, Y, Z : Integer) return Integer is
   begin
      if Y < X then
         return Tak(Tak(X - 1, Y, Z), Tak(Y - 1, Z, X), Tak(Z - 1, X, Y));
      else
         return Z;
      end if;
   end Tak;

begin
   for It in 1 .. Iters loop
      T0 := Calendar.Clock;
      Res := Fib(33) + Tak(24, 16, 8);
      T1 := Calendar.Clock;
      Ms := Long_Float(Calendar."-"(T1, T0)) * 1000.0;
      if Ms < Best_Ms then
         Best_Ms := Ms;
      end if;
   end loop;
   Int_IO.Put(Res, Width => 0);
   Text_IO.Put(" ms=");
   Flt_IO.Put(Best_Ms, Fore => 1, Aft => 3, Exp => 0);
   Text_IO.New_Line;
end Bench06_Recursion;
