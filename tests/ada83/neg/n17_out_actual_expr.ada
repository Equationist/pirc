-- Negative test: P3-7.13 non-variable OUT actual (character literal & short-circuit)
procedure Neg_Out_Actual_Expr is
   procedure Set_B (X : out Boolean) is begin X := True; end Set_B;
   A, B : Boolean := False;
begin
   Set_B (A and then B);
end Neg_Out_Actual_Expr;
