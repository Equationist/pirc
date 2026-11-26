-- Negative test: P3-7.11 duplicate subtype declaration
procedure Neg_Dup_Subtype is
   subtype S is Integer range 1 .. 5;
   subtype S is Integer range 1 .. 10;
begin
   null;
end Neg_Dup_Subtype;
