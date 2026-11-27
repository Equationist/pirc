procedure N56_Out_Read_Actual is
   type Arr is array (1 .. 5) of Integer;
   A : Arr := (others => 0);
   procedure Inner (X : out Integer) is
   begin
      X := 1;
   end Inner;
   procedure Outer (I : out Integer) is
   begin
      Inner (A (I));
      I := 1;
   end Outer;
   K : Integer;
begin
   Outer (K);
end N56_Out_Read_Actual;
