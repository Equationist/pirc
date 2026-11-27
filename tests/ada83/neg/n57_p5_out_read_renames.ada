procedure N57_Out_Read_Renames is
   type Arr is array (1 .. 5) of Integer;
   A : Arr := (others => 0);
   procedure P (I : out Integer) is
      X : Integer renames A (I);
   begin
      X := 2;
      I := 1;
   end P;
   K : Integer;
begin
   P (K);
end N57_Out_Read_Renames;
