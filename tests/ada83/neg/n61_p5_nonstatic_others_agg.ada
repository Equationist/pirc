procedure N61_Nonstatic_Others_Agg is
   N : Integer := 5;
   subtype Dyn_Idx is Integer range 1 .. N;
   type A is array (Dyn_Idx) of Integer;
   V : A := (1 => 10, others => 20);
begin
   null;
end N61_Nonstatic_Others_Agg;
