-- Negative test: P3-7.14 non-discrete array index subtype
procedure Neg_Non_Discrete_Idx is
   type Bad_Arr is array (Float range <>) of Integer;
begin
   null;
end Neg_Non_Discrete_Idx;
