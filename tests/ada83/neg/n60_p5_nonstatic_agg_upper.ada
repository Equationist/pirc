procedure N60_Nonstatic_Agg_Upper is
   N : Integer := 3;
   type A is array (1 .. 5) of Integer;
   V : A := (1 .. N => 1, others => 2);
begin
   null;
end N60_Nonstatic_Agg_Upper;
