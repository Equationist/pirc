-- Negative test: P3-5.15 single-file circular pragma ELABORATE rejection
package Cyc_A is
   function F return Integer;
end Cyc_A;

with Cyc_A;
pragma ELABORATE (Cyc_A);
package Cyc_B is
   function G return Integer;
end Cyc_B;

with Cyc_B;
pragma ELABORATE (Cyc_B);
package body Cyc_A is
   function F return Integer is begin return Cyc_B.G; end F;
end Cyc_A;

package body Cyc_B is
   function G return Integer is begin return Cyc_A.F; end G;
end Cyc_B;

with Cyc_A, Cyc_B;
procedure Neg_Cyc_Elab is
   X : Integer := Cyc_A.F;
begin
   if X = 0 then null; end if;
end Neg_Cyc_Elab;
