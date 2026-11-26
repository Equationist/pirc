-- Negative test: P3-6.2 enum rep code exceeding prior 'Size clause
procedure Neg_Enum_Rep_Size is
   type E is (A, B, C);
   for E'Size use 4;
   for E use (A => 0, B => 1, C => 100);
begin
   null;
end Neg_Enum_Rep_Size;
