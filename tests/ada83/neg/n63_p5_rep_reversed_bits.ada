procedure N63_Rep_Reversed_Bits is
   type R is record
      X : Integer;
   end record;
   for R use record
      X at 0 range 7 .. 0;
   end record;
begin
   null;
end N63_Rep_Reversed_Bits;
