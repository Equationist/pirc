-- Negative test: P3-7.16 rep clause after derived type freezes parent
procedure Neg_Rep_After_Der is
   type T is range 0 .. 15;
   type D is new T;
   for T'Size use 8;
begin
   null;
end Neg_Rep_After_Der;
