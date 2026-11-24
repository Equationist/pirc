with Text_IO;
procedure Test02_Nested is
   package Int_IO is new Text_IO.Integer_IO(Integer);

   Outer_Sum : Integer := 0;
   Scale     : Integer := 3;
   R1  : Integer := 0;
   Acc : Integer := 0;

   procedure Level1(A : Integer; Out_Val : out Integer) is
      L1_Var : Integer := A * 2;
      Tmp    : Integer := 0;

      procedure Level2(B : Integer) is
         L2_Var : Integer := B + 1;

         function Level3(C : Integer) return Integer is
         begin
            Outer_Sum := Outer_Sum + L1_Var + L2_Var + C;
            L1_Var := L1_Var + 1;
            return (L1_Var + L2_Var) * Scale + C;
         end Level3;
      begin
         Tmp := Level3(B * 10);
         L2_Var := L2_Var + 5;
         Tmp := Tmp + Level3(1);
      end Level2;
   begin
      Level2(A + 1);
      Out_Val := Tmp + L1_Var;
   end Level1;

   procedure Rec_Accum(N : Integer; Target : in out Integer) is
      procedure Step is
      begin
         Target := Target + N;
      end Step;
   begin
      if N > 0 then
         Step;
         Rec_Accum(N - 1, Target);
      end if;
   end Rec_Accum;
begin
   Level1(5, R1);
   Text_IO.Put("R1="); Int_IO.Put(R1, Width => 0); Text_IO.New_Line;
   Text_IO.Put("Outer_Sum="); Int_IO.Put(Outer_Sum, Width => 0); Text_IO.New_Line;
   Rec_Accum(10, Acc);
   Text_IO.Put("Acc="); Int_IO.Put(Acc, Width => 0); Text_IO.New_Line;
end Test02_Nested;
