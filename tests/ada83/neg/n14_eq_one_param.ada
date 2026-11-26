-- Negative test: P3-7.13 1-param "=" operator
procedure Neg_Eq_One_Param is
   package P is
      type L is limited private;
      function "=" (A : L) return Boolean;
   private
      type L is new Integer;
   end P;
   package body P is
      function "=" (A : L) return Boolean is begin return True; end "=";
   end P;
begin
   null;
end Neg_Eq_One_Param;
