package Math_Pkg is
   type Vec2 is record
      X : Integer;
      Y : Integer;
   end record;

   Counter : Integer := 100;

   function "+"(A, B : Vec2) return Vec2;
   function Dot(A, B : Vec2) return Integer;
   function Dot(A, B : Integer) return Integer;
   procedure Bump(Amount : Integer);
end Math_Pkg;
