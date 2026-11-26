-- Negative test: P3-7.11 calling uninstantiated generic subprogram
procedure Neg_Call_Uninst_Gen is
   generic
   procedure G;
   procedure G is begin null; end G;
begin
   G;
end Neg_Call_Uninst_Gen;
