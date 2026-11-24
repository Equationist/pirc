with Text_IO;
with Calendar;
procedure Bench02_Spectral is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Flt_IO is new Text_IO.Float_IO(Long_Float);

   N     : constant Integer := 400;
   Iters : constant Integer := 7;
   type Real_Arr is array (Integer range <>) of Long_Float;
   subtype Vec is Real_Arr(0 .. N - 1);

   U, V, Tmp : Vec;
   T0, T1 : Calendar.Time;
   Ms : Long_Float;
   Best_Ms : Long_Float := 1.0E9;
   Res : Long_Float := 0.0;

   function Sqrt_Newton(X : Long_Float) return Long_Float is
      G : Long_Float;
   begin
      if X <= 0.0 then
         return 0.0;
      end if;
      G := X * 0.5 + 0.5;
      for K in 1 .. 14 loop
         G := 0.5 * (G + X / G);
      end loop;
      return G;
   end Sqrt_Newton;

   function Eval_A(I, J : Integer) return Long_Float is
      S : Integer := I + J;
   begin
      return 1.0 / Long_Float((S * (S + 1)) / 2 + I + 1);
   end Eval_A;

   procedure Mult_Av(Src : Vec; Dst : in out Vec) is
      Sum : Long_Float;
   begin
      for I in 0 .. N - 1 loop
         Sum := 0.0;
         for J in 0 .. N - 1 loop
            Sum := Sum + Eval_A(I, J) * Src(J);
         end loop;
         Dst(I) := Sum;
      end loop;
   end Mult_Av;

   procedure Mult_Atv(Src : Vec; Dst : in out Vec) is
      Sum : Long_Float;
   begin
      for I in 0 .. N - 1 loop
         Sum := 0.0;
         for J in 0 .. N - 1 loop
            Sum := Sum + Eval_A(J, I) * Src(J);
         end loop;
         Dst(I) := Sum;
      end loop;
   end Mult_Atv;

   function Run_Once return Long_Float is
      Vbv, Vv : Long_Float := 0.0;
   begin
      for I in 0 .. N - 1 loop
         U(I) := 1.0;
         V(I) := 0.0;
      end loop;
      for Step in 1 .. 10 loop
         Mult_Av(U, Tmp);
         Mult_Atv(Tmp, V);
         Mult_Av(V, Tmp);
         Mult_Atv(Tmp, U);
      end loop;
      for I in 0 .. N - 1 loop
         Vbv := Vbv + U(I) * V(I);
         Vv  := Vv  + V(I) * V(I);
      end loop;
      return Sqrt_Newton(Vbv / Vv);
   end Run_Once;

begin
   for It in 1 .. Iters loop
      T0 := Calendar.Clock;
      Res := Run_Once;
      T1 := Calendar.Clock;
      Ms := Long_Float(Calendar."-"(T1, T0)) * 1000.0;
      if Ms < Best_Ms then
         Best_Ms := Ms;
      end if;
   end loop;
   Int_IO.Put(Integer(Res * 1_000_000.0), Width => 0);
   Text_IO.Put(" ms=");
   Flt_IO.Put(Best_Ms, Fore => 1, Aft => 3, Exp => 0);
   Text_IO.New_Line;
end Bench02_Spectral;
