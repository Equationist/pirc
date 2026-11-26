-- Negative test: P3-7.12 limited OUT parameter inside generic package (LRM 7.4.4(4))
procedure Neg_Lim_Out_Gen is
   task type T;
   task body T is begin null; end T;
   generic
   package G is
      procedure Bad (X : out T);
   end G;
   package body G is
      procedure Bad (X : out T) is begin null; end Bad;
   end G;
begin
   null;
end Neg_Lim_Out_Gen;
