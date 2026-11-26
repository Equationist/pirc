-- Pass 4 negative test: parenthesized variable on LHS of assignment (LRM 5.2)
procedure N47_Paren_Lhs is
   X : Integer := 0;
begin
   (X) := 1;
end N47_Paren_Lhs;
