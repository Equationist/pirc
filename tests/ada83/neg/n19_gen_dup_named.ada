-- Negative test: P3-7.14 duplicate named generic association
procedure Neg_Gen_Dup_Named is
   generic
      X : Integer;
      Y : Integer := 1;
   package G is end G;
   package I is new G (X => 1, X => 2);
begin
   null;
end Neg_Gen_Dup_Named;
