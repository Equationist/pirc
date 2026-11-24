with Text_IO;
procedure Test03_Arrays is
   package Int_IO is new Text_IO.Integer_IO(Integer);

   type Int_Vec is array (Integer range <>) of Integer;
   subtype Neg_Arr is Int_Vec(-3 .. 3);
   type Mat2D is array (Integer range 1 .. 3, Integer range -1 .. 1) of Integer;

   A : Neg_Arr := (-3 => 10, -2 => 20, -1 => 30, 0 => 40, 1 .. 3 => 5);
   B : Neg_Arr;
   M : Mat2D;
   N : Integer := 6;
   Total : Integer := 0;
   Oob_Caught : Integer := 0;

   function Sum_Vec(V : Int_Vec) return Integer is
      S : Integer := 0;
   begin
      for I in V'Range loop
         S := S + V(I);
      end loop;
      return S;
   end Sum_Vec;

   procedure Scale_Vec(V : in out Int_Vec; K : Integer) is
   begin
      for I in V'First .. V'Last loop
         V(I) := V(I) * K;
      end loop;
   end Scale_Vec;
begin
   Text_IO.Put("A_first="); Int_IO.Put(A'First, Width => 0); Text_IO.New_Line;
   Text_IO.Put("A_last=");  Int_IO.Put(A'Last, Width => 0); Text_IO.New_Line;
   Text_IO.Put("A_len=");   Int_IO.Put(A'Length, Width => 0); Text_IO.New_Line;
   Text_IO.Put("sum_A=");   Int_IO.Put(Sum_Vec(A), Width => 0); Text_IO.New_Line;

   -- Slice passed to unconstrained parameter
   Text_IO.Put("slice=");   Int_IO.Put(Sum_Vec(A(-2 .. 1)), Width => 0); Text_IO.New_Line;

   -- Whole-array copy and equality
   B := A;
   if A = B then
      Text_IO.Put_Line("eq=1");
   else
      Text_IO.Put_Line("eq=0");
   end if;
   Scale_Vec(B(-1 .. 2), 2);
   Text_IO.Put("sum_B="); Int_IO.Put(Sum_Vec(B), Width => 0); Text_IO.New_Line;
   if A /= B then
      Text_IO.Put_Line("neq=1");
   end if;

   -- 2D array with negative second dimension
   for R in 1 .. 3 loop
      for C in Integer range -1 .. 1 loop
         M(R, C) := R * 10 + (C + 2);
      end loop;
   end loop;
   for R in 1 .. 3 loop
      for C in Integer range -1 .. 1 loop
         Total := Total + M(R, C);
      end loop;
   end loop;
   Text_IO.Put("mat="); Int_IO.Put(Total, Width => 0); Text_IO.New_Line;

   -- Dynamic-bound array inside declare block (tests alloca_dyn + stacksave/stackrestore)
   declare
      Dyn : Int_Vec(1 .. N);
   begin
      for I in Dyn'Range loop
         Dyn(I) := I * I;
      end loop;
      Text_IO.Put("dyn="); Int_IO.Put(Sum_Vec(Dyn), Width => 0); Text_IO.New_Line;
   end;

   -- Out-of-bounds index raises Constraint_Error
   begin
      Total := A(4);
      Oob_Caught := 0;
   exception
      when Constraint_Error =>
         Oob_Caught := 1;
   end;
   Text_IO.Put("oob="); Int_IO.Put(Oob_Caught, Width => 0); Text_IO.New_Line;
end Test03_Arrays;
