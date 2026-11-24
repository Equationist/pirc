with Text_IO;
procedure Test05_GenCtrl is
   package Int_IO is new Text_IO.Integer_IO(Integer);

   type Color is (Red, Green, Blue, Yellow);

   X : Integer := 11;
   Y : Integer := 22;
   Acc : Integer := 0;
   C : Color := Green;
   D : Integer := 0;

   generic
      type T is private;
   procedure Swap(A, B : in out T);

   procedure Swap(A, B : in out T) is
      Tmp : T := A;
   begin
      A := B;
      B := Tmp;
   end Swap;

   procedure Swap_Int is new Swap(Integer);

   function Classify(N : Integer) return Integer is
   begin
      case N is
         when 1 | 3 | 5 =>
            return 10;
         when 10 .. 20 =>
            return 20;
         when 100 .. 1000 =>
            return 30;
         when others =>
            return 40;
      end case;
   end Classify;
begin
   Swap_Int(X, Y);
   Text_IO.Put("swap="); Int_IO.Put(X, Width => 0);
   Text_IO.Put(","); Int_IO.Put(Y, Width => 0); Text_IO.New_Line;

   -- Case statement
   Text_IO.Put("case=");
   Int_IO.Put(Classify(3) + Classify(15) + Classify(500) + Classify(9999), Width => 0);
   Text_IO.New_Line;

   -- Reverse for loop
   for I in reverse 1 .. 5 loop
      Acc := Acc * 10 + I;
   end loop;
   Text_IO.Put("rev="); Int_IO.Put(Acc, Width => 0); Text_IO.New_Line;

   -- Wrap-safe loop reaching Integer'Last!
   Acc := 0;
   for I in Integer'Last - 2 .. Integer'Last loop
      Acc := Acc + 1;
   end loop;
   Text_IO.Put("max_loop="); Int_IO.Put(Acc, Width => 0); Text_IO.New_Line;

   -- Labeled nested loops + exit Outer
   Acc := 0;
   Outer :
   for I in 1 .. 10 loop
      for J in 1 .. 10 loop
         Acc := Acc + 1;
         exit Outer when I = 3 and then J = 4;
      end loop;
   end loop Outer;
   Text_IO.Put("exit_outer="); Int_IO.Put(Acc, Width => 0); Text_IO.New_Line;

   -- Short-circuit prevents divide-by-zero
   D := 0;
   if D /= 0 and then (100 / D > 5) then
      Acc := 999;
   elsif D = 0 or else (100 / D > 5) then
      Acc := 42;
   end if;
   Text_IO.Put("sc="); Int_IO.Put(Acc, Width => 0); Text_IO.New_Line;

   -- Membership + enum attributes
   if X in 20 .. 25 and then Color'Succ(C) = Blue and then Color'Pos(Yellow) = 3 then
      Text_IO.Put_Line("enum_mem=1");
   else
      Text_IO.Put_Line("enum_mem=0");
   end if;
end Test05_GenCtrl;
