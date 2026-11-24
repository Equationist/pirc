with Text_IO;
with Unchecked_Deallocation;
procedure Test04_Records is
   package Int_IO is new Text_IO.Integer_IO(Integer);

   type Point is record
      X : Integer;
      Y : Integer;
   end record;

   type Shape_Kind is (Circle, Rect);
   type Shape(Kind : Shape_Kind := Circle) is record
      Origin : Point;
      case Kind is
         when Circle =>
            Radius : Integer;
         when Rect =>
            W : Integer;
            H : Integer;
      end case;
   end record;

   type Node;
   type Node_Ptr is access Node;
   type Node is record
      Val  : Integer;
      Next : Node_Ptr;
   end record;

   procedure Free_Node is new Unchecked_Deallocation(Node, Node_Ptr);

   P1 : Point := (X => 3, Y => 4);
   P2 : Point := (10, 20);
   P3 : Point;
   S1 : Shape(Circle);
   S2 : Shape(Rect);
   Head : Node_Ptr := null;
   Cur  : Node_Ptr;
   Sum  : Integer := 0;
   Null_Caught : Integer := 0;

   function Add_Points(A, B : Point) return Point is
   begin
      return (X => A.X + B.X, Y => A.Y + B.Y);
   end Add_Points;

   function Area(S : Shape) return Integer is
   begin
      case S.Kind is
         when Circle =>
            return 3 * S.Radius * S.Radius;
         when Rect =>
            return S.W * S.H;
      end case;
   end Area;
begin
   P3 := Add_Points(P1, P2);
   Text_IO.Put("P3="); Int_IO.Put(P3.X, Width => 0);
   Text_IO.Put(","); Int_IO.Put(P3.Y, Width => 0); Text_IO.New_Line;

   S1.Origin := P1; S1.Radius := 5;
   S2.Origin := P2; S2.W := 6; S2.H := 7;
   Text_IO.Put("A1="); Int_IO.Put(Area(S1), Width => 0); Text_IO.New_Line;
   Text_IO.Put("A2="); Int_IO.Put(Area(S2), Width => 0); Text_IO.New_Line;

   -- Build linked list on heap
   for I in 1 .. 5 loop
      Head := new Node'(Val => I * 10, Next => Head);
   end loop;
   Cur := Head;
   while Cur /= null loop
      Sum := Sum + Cur.Val;
      Cur := Cur.Next;
   end loop;
   Text_IO.Put("list="); Int_IO.Put(Sum, Width => 0); Text_IO.New_Line;

   -- Free linked list
   while Head /= null loop
      Cur := Head.Next;
      Free_Node(Head);
      Head := Cur;
   end loop;

   -- Null dereference check
   begin
      Sum := Head.Val;
      Null_Caught := 0;
   exception
      when Constraint_Error =>
         Null_Caught := 1;
   end;
   Text_IO.Put("null_chk="); Int_IO.Put(Null_Caught, Width => 0); Text_IO.New_Line;
end Test04_Records;
