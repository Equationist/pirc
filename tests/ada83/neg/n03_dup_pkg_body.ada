-- Negative test: P3-7.11 duplicate package body
procedure Neg_Dup_Pkg_Body is
   package P is
      procedure F;
   end P;
   package body P is
      procedure F is begin null; end F;
   end P;
   package body P is
      procedure F is begin null; end F;
   end P;
begin
   P.F;
end Neg_Dup_Pkg_Body;
