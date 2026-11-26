-- Negative test: P3-7.15 duplicate choices in array aggregate
procedure Neg_Agg_Dup_Choice is
   type A4 is array (1 .. 4) of Integer;
   X : A4 := (1 .. 2 => 10, 2 .. 3 => 20, others => 0);
begin
   null;
end Neg_Agg_Dup_Choice;
