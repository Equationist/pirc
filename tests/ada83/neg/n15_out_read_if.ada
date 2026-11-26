-- Negative test: P3-7.13 OUT parameter read in if condition
procedure Neg_Out_Read_If is
   procedure Bad (X : out Boolean) is
   begin
      if X then
         null;
      end if;
      X := True;
   end Bad;
begin
   null;
end Neg_Out_Read_If;
