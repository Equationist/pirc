-- Negative test: P3-7.15 non-static number declaration
procedure Neg_Non_Static_Num_Decl is
   V : Integer := 5;
   N : constant := V;
begin
   null;
end Neg_Non_Static_Num_Decl;
