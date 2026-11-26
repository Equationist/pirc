-- Negative test: P3-7.13 operator default parameter & wrong arity
procedure Neg_Op_Default is
   function "+" (A, B : Integer := 0) return Integer is
   begin
      return A + B;
   end "+";
begin
   null;
end Neg_Op_Default;
