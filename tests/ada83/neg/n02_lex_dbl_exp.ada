-- Negative test: Pass 1/2 lexer double exponent (1E10E-2)
procedure Neg_Lex_Dbl_Exp is
   X : Integer := 1E2E3;
begin
   null;
end Neg_Lex_Dbl_Exp;
