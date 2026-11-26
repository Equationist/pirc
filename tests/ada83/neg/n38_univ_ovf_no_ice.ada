-- Negative test: P3-7.5 static universal overflow (2.0 ** 4000) must not crash semphase
procedure Neg_Univ_Ovf_No_Ice is
   N : constant := 2.0 ** 4000;
begin
   null;
end Neg_Univ_Ovf_No_Ice;
