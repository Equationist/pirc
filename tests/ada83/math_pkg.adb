package body Math_Pkg is
   function "+"(A, B : Vec2) return Vec2 is
   begin
      return (X => A.X + B.X, Y => A.Y + B.Y);
   end "+";

   function Dot(A, B : Vec2) return Integer is
   begin
      return A.X * B.X + A.Y * B.Y;
   end Dot;

   function Dot(A, B : Integer) return Integer is
   begin
      return A * B;
   end Dot;

   procedure Bump(Amount : Integer) is
   begin
      Counter := Counter + Amount;
   end Bump;
begin
   Counter := Counter + 7;
end Math_Pkg;
