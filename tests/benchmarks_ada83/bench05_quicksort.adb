with Text_IO;
with Calendar;
procedure Bench05_Quicksort is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Flt_IO is new Text_IO.Float_IO(Long_Float);

   N     : constant Integer := 100_000;
   Iters : constant Integer := 9;
   type Int_Arr is array (Integer range <>) of Integer;
   subtype Data_Arr is Int_Arr(1 .. N);
   A : Data_Arr;

   T0, T1 : Calendar.Time;
   Ms : Long_Float;
   Best_Ms : Long_Float := 1.0E9;
   Chk : Integer := 0;

   procedure Fill_Data is
      Seed : Long_Integer := 1234567;
   begin
      for I in 1 .. N loop
         Seed := (Seed * 1103515245 + 12345) mod 2147483647;
         A(I) := Integer(Seed mod 100_000);
      end loop;
   end Fill_Data;

   procedure QSort(Arr : in out Int_Arr; Lo, Hi : Integer) is
      I, J, Pivot, Tmp : Integer;
   begin
      if Lo >= Hi then
         return;
      end if;
      I := Lo;
      J := Hi;
      Pivot := Arr(Lo + (Hi - Lo) / 2);
      while I <= J loop
         while Arr(I) < Pivot loop
            I := I + 1;
         end loop;
         while Arr(J) > Pivot loop
            J := J - 1;
         end loop;
         if I <= J then
            Tmp := Arr(I);
            Arr(I) := Arr(J);
            Arr(J) := Tmp;
            I := I + 1;
            J := J - 1;
         end if;
      end loop;
      if Lo < J then
         QSort(Arr, Lo, J);
      end if;
      if I < Hi then
         QSort(Arr, I, Hi);
      end if;
   end QSort;

begin
   for It in 1 .. Iters loop
      Fill_Data;
      T0 := Calendar.Clock;
      QSort(A, 1, N);
      T1 := Calendar.Clock;
      Ms := Long_Float(Calendar."-"(T1, T0)) * 1000.0;
      if Ms < Best_Ms then
         Best_Ms := Ms;
      end if;
      Chk := A(1) + A(N / 2) * 7 + A(N) * 13;
   end loop;
   Int_IO.Put(Chk, Width => 0);
   Text_IO.Put(" ms=");
   Flt_IO.Put(Best_Ms, Fore => 1, Aft => 3, Exp => 0);
   Text_IO.New_Line;
end Bench05_Quicksort;
