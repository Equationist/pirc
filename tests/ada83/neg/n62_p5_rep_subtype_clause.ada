procedure N62_Rep_Subtype_Clause is
   type E is (A, B);
   subtype SE is E;
   for SE use (A => 1, B => 2);
begin
   null;
end N62_Rep_Subtype_Clause;
