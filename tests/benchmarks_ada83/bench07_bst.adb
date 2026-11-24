with Text_IO;
with Calendar;
procedure Bench07_Bst is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Flt_IO is new Text_IO.Float_IO(Long_Float);

   type Node;
   type Node_Ptr is access Node;
   type Node is record
      Key   : Integer;
      Val   : Integer;
      Left  : Node_Ptr;
      Right : Node_Ptr;
   end record;

   Root : Node_Ptr := null;
   N_Nodes : constant Integer := 30_000;
   Lookups : constant Integer := 300_000;
   Iters   : constant Integer := 9;
   T0, T1  : Calendar.Time;
   Ms      : Long_Float;
   Best_Ms : Long_Float := 1.0E9;
   Sum     : Integer := 0;

   procedure Insert(Tree : in out Node_Ptr; K, V : Integer) is
      Cur : Node_Ptr;
   begin
      if Tree = null then
         Tree := new Node'(Key => K, Val => V, Left => null, Right => null);
         return;
      end if;
      Cur := Tree;
      loop
         if K < Cur.Key then
            if Cur.Left = null then
               Cur.Left := new Node'(Key => K, Val => V, Left => null, Right => null);
               return;
            end if;
            Cur := Cur.Left;
         elsif K > Cur.Key then
            if Cur.Right = null then
               Cur.Right := new Node'(Key => K, Val => V, Left => null, Right => null);
               return;
            end if;
            Cur := Cur.Right;
         else
            Cur.Val := V;
            return;
         end if;
      end loop;
   end Insert;

   function Lookup(Tree : Node_Ptr; K : Integer) return Integer is
      Cur : Node_Ptr := Tree;
   begin
      while Cur /= null loop
         if K < Cur.Key then
            Cur := Cur.Left;
         elsif K > Cur.Key then
            Cur := Cur.Right;
         else
            return Cur.Val;
         end if;
      end loop;
      return 0;
   end Lookup;

   procedure Build_Tree is
      Seed : Long_Integer := 987654321;
      K    : Integer;
   begin
      for I in 1 .. N_Nodes loop
         Seed := (Seed * 1103515245 + 12345) mod 2147483647;
         K := Integer(Seed mod 1_000_000);
         Insert(Root, K, I mod 97);
      end loop;
   end Build_Tree;

   function Run_Lookups return Integer is
      Seed : Long_Integer := 135792468;
      K    : Integer;
      Acc  : Integer := 0;
   begin
      for I in 1 .. Lookups loop
         Seed := (Seed * 1103515245 + 12345) mod 2147483647;
         K := Integer(Seed mod 1_000_000);
         Acc := (Acc + Lookup(Root, K)) mod 1_000_000_007;
      end loop;
      return Acc;
   end Run_Lookups;

begin
   Build_Tree;
   for It in 1 .. Iters loop
      T0 := Calendar.Clock;
      Sum := Run_Lookups;
      T1 := Calendar.Clock;
      Ms := Long_Float(Calendar."-"(T1, T0)) * 1000.0;
      if Ms < Best_Ms then
         Best_Ms := Ms;
      end if;
   end loop;
   Int_IO.Put(Sum, Width => 0);
   Text_IO.Put(" ms=");
   Flt_IO.Put(Best_Ms, Fore => 1, Aft => 3, Exp => 0);
   Text_IO.New_Line;
end Bench07_Bst;
