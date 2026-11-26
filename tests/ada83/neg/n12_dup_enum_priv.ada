-- Negative test: P3-7.12 duplicate enum literal in private completion
procedure Neg_Dup_Enum_Priv is
   package P is
      type E is private;
   private
      type E is (A, A);
   end P;
begin
   null;
end Neg_Dup_Enum_Priv;
