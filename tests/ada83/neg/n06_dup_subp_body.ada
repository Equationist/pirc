-- Negative test: P3-7.11 duplicate subprogram body
procedure Neg_Dup_Subp_Body is
   procedure P;
   procedure P is begin null; end P;
   procedure P is begin null; end P;
begin
   P;
end Neg_Dup_Subp_Body;
