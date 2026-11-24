-- Test 09: Returned Arrays (Unconstrained, Dynamic-Bound, 2D, Aggregates,
--          Concatenation) & Secondary Stack Lifetime Management
with Text_IO;
procedure Test09_Returned_Arrays is
   package Int_IO is new Text_IO.Integer_IO(Integer);

   type Int_Arr is array (Integer range <>) of Integer;
   type Mat2D   is array (Integer range <>, Integer range <>) of Integer;
   Sum : Integer := 0;

   -- 1. Returning unconstrained String built from dynamic local array
   function Repeat_Char(Ch : Character; N : Integer) return String is
      Res : String(1 .. N);
   begin
      for I in 1 .. N loop
         Res(I) := Ch;
      end loop;
      return Res;
   end Repeat_Char;

   -- 2. Recursive unconstrained String return using slices + `&` concatenation
   function Reverse_Str(S : String) return String is
   begin
      if S'Length <= 1 then
         return S;
      else
         return Reverse_Str(S(S'First + 1 .. S'Last)) & S(S'First);
      end if;
   end Reverse_Str;

   -- 3. Returning unconstrained 1D array with negative lower bound
   function Make_Range(Lo, Hi : Integer) return Int_Arr is
      A : Int_Arr(Lo .. Hi);
   begin
      for I in Lo .. Hi loop
         A(I) := I * 10;
      end loop;
      return A;
   end Make_Range;

   -- 4. Returning unconstrained array aggregates (positional and dynamic range)
   function Make_Positional return Int_Arr is
   begin
      return (11, 22, 33, 44);
   end Make_Positional;

   function Make_Filled(N, Val : Integer) return Int_Arr is
   begin
      return (1 .. N => Val);
   end Make_Filled;

   -- 5. Returning unconstrained 2D array
   function Make_Mat(R, C : Integer) return Mat2D is
      M : Mat2D(1 .. R, 1 .. C);
   begin
      for I in 1 .. R loop
         for J in 1 .. C loop
            M(I, J) := I * 10 + J;
         end loop;
      end loop;
      return M;
   end Make_Mat;

   -- 6. Dynamic-bound constrained subtype return
   function Make_Dyn_Subtype(N : Integer) return Int_Arr is
      subtype Dyn_Vec is Int_Arr(1 .. N);
      function Build return Dyn_Vec is
         V : Dyn_Vec;
      begin
         for I in 1 .. N loop
            V(I) := I * I;
         end loop;
         return V;
      end Build;
   begin
      return Build;
   end Make_Dyn_Subtype;

   -- 7. Returning unconstrained array from inside an exception-raising path
   function Maybe_Make(N : Integer; Fail : Boolean) return String is
      Tmp : constant String := Repeat_Char('X', N) & Repeat_Char('Y', N);
   begin
      if Fail then
         raise Constraint_Error;
      end if;
      return Tmp;
   end Maybe_Make;

   -- 8. O(1) Secondary Stack lifetime stress test in a tight loop + early exit
   function Loop_Stress(Iters : Integer) return Integer is
      Total : Integer := 0;
   begin
      for I in 1 .. Iters loop
         declare
            S : constant String := Repeat_Char('A', 64) & Repeat_Char('B', 64);
         begin
            Total := Total + S'Length;
            if I = Iters then
               exit;
            end if;
         end;
      end loop;
      return Total;
   end Loop_Stress;

begin
   -- String returns, indexing, attributes, and recursion
   declare
      S1 : constant String := Repeat_Char('*', 5);
      S2 : constant String := Reverse_Str("Ada83!");
      S3 : String(1 .. 5) := "hello";
      S4 : String(1 .. 5) := Repeat_Char('x', 5);
   begin
      S4 := S3;
      S3(1) := 'H';
      S4(1) := 'J';
      Text_IO.Put_Line("s1=" & S1 & " rev=" & S2 & " s3=" & S3 & " s4=" & S4);
      Text_IO.Put("idx=");
      Text_IO.Put(Repeat_Char('Q', 4)(3));
      Text_IO.Put(" len=");
      declare
         S12 : constant String := S1 & S2;
      begin
         Int_IO.Put(S12'Length, Width => 0);
      end;
      Text_IO.New_Line;
   end;

   -- Negative-bound 1D array return
   declare
      A : constant Int_Arr := Make_Range(-3, 2);
   begin
      Sum := 0;
      for I in A'Range loop
         Sum := Sum + A(I);
      end loop;
      Text_IO.Put("neg_arr=");
      Int_IO.Put(A'First, Width => 0); Text_IO.Put("..");
      Int_IO.Put(A'Last, Width => 0); Text_IO.Put(":");
      Int_IO.Put(Sum, Width => 0);
      Text_IO.New_Line;
   end;

   -- Aggregate returns & dynamic subtype return
   declare
      P : constant Int_Arr := Make_Positional;
      F : constant Int_Arr := Make_Filled(5, 7);
      D : constant Int_Arr := Make_Dyn_Subtype(4);
   begin
      Text_IO.Put("agg=");
      Int_IO.Put(P(P'First) + P(P'Last), Width => 0); Text_IO.Put(",");
      Int_IO.Put(P'Length, Width => 0); Text_IO.Put(",");
      Int_IO.Put(F(1) + F(5), Width => 0); Text_IO.Put(",");
      Int_IO.Put(D(1) + D(4), Width => 0);
      Text_IO.New_Line;
   end;

   -- 2D unconstrained array return
   declare
      M : constant Mat2D := Make_Mat(3, 4);
   begin
      Sum := 0;
      for I in M'Range(1) loop
         for J in M'Range(2) loop
            Sum := Sum + M(I, J);
         end loop;
      end loop;
      Text_IO.Put("mat2d=");
      Int_IO.Put(M'First(1), Width => 0); Text_IO.Put("..");
      Int_IO.Put(M'Last(2), Width => 0); Text_IO.Put(":");
      Int_IO.Put(M(2, 3), Width => 0); Text_IO.Put(":");
      Int_IO.Put(Sum, Width => 0);
      Text_IO.New_Line;
   end;

   -- Exception unwind reclaiming secondary stack allocations
   declare
      Caught : Integer := 0;
   begin
      for I in 1 .. 500 loop
         begin
            declare
               S : constant String := Maybe_Make(64, (I mod 2) = 0);
            begin
               Caught := Caught + S'Length;
            end;
         exception
            when Constraint_Error =>
               Caught := Caught + 1;
         end;
      end loop;
      Text_IO.Put("exc_ss=");
      Int_IO.Put(Caught, Width => 0);
      Text_IO.New_Line;
   end;

   -- 50,000-iteration loop stress test (~6.4 MB total allocated & reclaimed in O(1) SS space)
   Text_IO.Put("stress=");
   Int_IO.Put(Loop_Stress(50_000), Width => 0);
   Text_IO.New_Line;
end Test09_Returned_Arrays;
