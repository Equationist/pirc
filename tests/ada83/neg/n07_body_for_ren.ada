-- Negative test: P3-7.11 body for renamed subprogram
procedure Neg_Body_For_Ren is
   procedure A is begin null; end A;
   procedure B renames A;
   procedure B is begin null; end B;
begin
   B;
end Neg_Body_For_Ren;
