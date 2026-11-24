with Text_IO;
with Calendar;
procedure Bench01_Sieve is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Flt_IO is new Text_IO.Float_IO(Long_Float);

   Limit  : constant Integer := 200_000;
   Passes : constant Integer := 15;
   Iters  : constant Integer := 9;
   type Bool_Arr is array (Integer range 0 .. Limit) of Boolean;
   Flags : Bool_Arr;
   Count : Integer := 0;
   T0, T1 : Calendar.Time;
   Ms : Long_Float;
   Best_Ms : Long_Float := 1.0E9;
   Chk : Integer := 0;

   function Run_Sieve return Integer is
      P, J, C : Integer;
   begin
      for I in 0 .. Limit loop
         Flags(I) := True;
      end loop;
      Flags(0) := False;
      Flags(1) := False;
      P := 2;
      while P * P <= Limit loop
         if Flags(P) then
            J := P * P;
            while J <= Limit loop
               Flags(J) := False;
               J := J + P;
            end loop;
         end if;
         P := P + 1;
      end loop;
      C := 0;
      for I in 2 .. Limit loop
         if Flags(I) then
            C := C + 1;
         end if;
      end loop;
      return C;
   end Run_Sieve;

begin
   for It in 1 .. Iters loop
      T0 := Calendar.Clock;
      for Pass in 1 .. Passes loop
         Count := Run_Sieve;
      end loop;
      T1 := Calendar.Clock;
      Ms := Long_Float(Calendar."-"(T1, T0)) * 1000.0;
      if Ms < Best_Ms then
         Best_Ms := Ms;
      end if;
      Chk := Count;
   end loop;
   Int_IO.Put(Chk, Width => 0);
   Text_IO.Put(" ms=");
   Flt_IO.Put(Best_Ms, Fore => 1, Aft => 3, Exp => 0);
   Text_IO.New_Line;
end Bench01_Sieve;
