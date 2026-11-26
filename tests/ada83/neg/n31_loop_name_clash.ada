-- Negative test: P3-7.17 loop parameter clashing with inner loop name
procedure Neg_Loop_Name_Clash is
begin
   for I in 1 .. 3 loop
      I : loop
         exit I;
      end loop I;
   end loop;
end Neg_Loop_Name_Clash;
