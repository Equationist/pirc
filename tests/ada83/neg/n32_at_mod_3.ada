-- Negative test: P3-6.4 non-power-of-2 at mod 3 alignment clause
procedure Neg_At_Mod_3 is
   type R is record
      X : Integer;
   end record;
   for R use record
      at mod 3;
      X at 0 range 0 .. 31;
   end record;
begin
   null;
end Neg_At_Mod_3;
