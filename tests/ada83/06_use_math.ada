with Text_IO;
with Math_Pkg; use Math_Pkg;
procedure Use_Math is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   U : Vec2 := (3, 4);
   V : Vec2 := (5, 6);
   W : Vec2;
begin
   W := U + V;
   Bump(13);
   Text_IO.Put("W="); Int_IO.Put(W.X, Width => 0);
   Text_IO.Put(","); Int_IO.Put(W.Y, Width => 0); Text_IO.New_Line;
   Text_IO.Put("dotv="); Int_IO.Put(Dot(U, V), Width => 0); Text_IO.New_Line;
   Text_IO.Put("doti="); Int_IO.Put(Dot(7, 8), Width => 0); Text_IO.New_Line;
   Text_IO.Put("cnt=");  Int_IO.Put(Counter, Width => 0); Text_IO.New_Line;
end Use_Math;
