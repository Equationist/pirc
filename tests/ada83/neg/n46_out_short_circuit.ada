-- Pass 4 negative test: reading OUT parameter inside short-circuit op (LRM 6.2(5))
procedure N46_Out_Short_Circuit is
   procedure P (X : out Boolean) is
      B : Boolean;
   begin
      B := X and then True;
      X := B;
   end P;
begin
   null;
end N46_Out_Short_Circuit;
